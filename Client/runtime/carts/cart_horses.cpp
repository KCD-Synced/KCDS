// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "cart_horses.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "MinHook.h"

#include "../appearance/outfit.hpp"
#include "../core/log.hpp"
#include "../props/prop_entity.hpp"
#include "../replicas/remote.hpp"
#include "../core/natives.hpp"
#include "../npcs/npc_body.hpp"
#include "../npcs/npc_suppression.hpp"
#include "../replicas/motion_capture.hpp"
#include "../riding/native_horses.hpp"
#include "kcd2s/integration/guid.hpp"

namespace kcds::game_client {
namespace {

// How often a cart horse's use prompts are switched off again (seconds).
constexpr double kPromptsOffSeconds = 1.0;
// A spawn the game refused is tried again after this long (seconds).
constexpr double kRetrySeconds = 5.0;
// The movement request's type for a body walking at a velocity (CryEngine's character move
// types: 1 normal; the local player's own requests carry it, LOCAL MOTION type=1).
constexpr std::uint32_t kVelocityRequest = 1;
// The motion params the blend spaces read (MoveSpeed, TurnSpeed).
constexpr int kMoveSpeedParam = 0;
constexpr int kTurnSpeedParam = 1;
// Below this speed (metres a second) a cart horse stands; up to the next it walks, above it trots.
constexpr float kStandingSpeed = 0.2f;
constexpr float kWalkingSpeed = 2.35f;

// A horse whose place stays is put there again only when it is this far from it (metres, and the
// quaternions' dot product): the game settles its body a few centimetres off the place (on the
// ground, clear of the shafts, 8 cm measured), and putting it back every frame against that
// shook its mane's hair.
constexpr float kHoldSlack = 0.12f;
constexpr float kHoldTurnSlack = 0.99985f;  // 2 degrees
// How little the place may change between frames to count as staying (metres).
constexpr float kPlaceStays = 0.002f;
// IEntity GetWorldTM (slot 32: a 3x4 row-major matrix).
constexpr std::size_t kWorldTmSlot = 32;

enum class Gait { Stand, Walk, Trot, Back };

// The harness items (Libs/Tables/item/item__aux.xml).
constexpr char kCartBridle[] = "7bca8097-7d3c-4d5d-8de0-9c336de7761d";     // CartBridle01_m01
constexpr char kYokeLeft[] = "467f8539-39b0-475d-97d9-126f3036a3b2";       // YokeDouble01Left
constexpr char kYokeRight[] = "a880782f-37f7-4047-8ce7-37992109154c";      // YokeDouble01Right
constexpr char kYokeUniversal[] = "25423384-b6c9-45ff-a915-d8514e4629b1";  // YokeUniversal01
// The outfit keys of the cart horses, apart from the players' network ids.
constexpr std::uint32_t kOutfitKey = 0xC0000000u;

// The outfit field (appearance/outfit.hpp capture) of a harness: per piece its class id, any
// quality (0: the game gives these items its own) and full health.
std::string harness_outfit(HarnessSide side) {
  const char* yoke = kYokeUniversal;
  if (side == HarnessSide::Left) {
    yoke = kYokeLeft;
  } else if (side == HarnessSide::Right) {
    yoke = kYokeRight;
  }
  std::string outfit = "KO2";
  for (const char* piece : {yoke, kCartBridle}) {
    kcd2s::integration::Guid id{};
    if (!kcd2s::integration::parse_guid(piece, id)) {
      continue;
    }
    outfit.append(reinterpret_cast<const char*>(id.data()), id.size());
    outfit.push_back(char(0));
    outfit.push_back(char(0xFF));
    outfit.push_back(char(0xFF));
  }
  return outfit;
}

const char* clip_of(Gait gait) {
  switch (gait) {
    case Gait::Walk:
      return "2d_walk_turn_wagon";
    case Gait::Trot:
      return "2d_trot_turn";
    case Gait::Back:
      return "1d_walk_back_wagon";
    default:
      return nullptr;
  }
}

// The game's cart horse step reads this: the horse's actor, its side in
// the harness (a byte: 0 left, 1 right), the cart's entity and the modifier (a std::shared_ptr:
// the object and its count block).
struct ReinsStep {
  void* horse_actor = nullptr;
  std::uint64_t side = 0;
  void* cart_entity = nullptr;
  void* unused[4] = {};
  void* modifier = nullptr;
  void* modifier_count = nullptr;
};
static_assert(sizeof(ReinsStep) == 72);

// The modifier: its bone pairs (a vector of 32-byte entries, begin + 8, end + 16), the driver's
// entity id (+ 32) and the cart's (+ 36, set by the builder).
constexpr std::size_t kReinsPairsBegin = 8;
constexpr std::size_t kReinsPairsEnd = 16;
constexpr std::size_t kReinsDriver = 32;
// ISkeletonAnim::PushPoseModifier(layer, the modifier's shared_ptr, its name): the slot and the
// layer the game's step pushes it on.
constexpr std::size_t kPushPoseModifierSlot = 37;
constexpr std::uint32_t kReinsLayer = 3;

using CreateReins = bool(__fastcall*)(void* unused, void* out[2]);
using BuildReins = void*(__fastcall*)(ReinsStep* step, char side);
using PushPoseModifier = bool(__fastcall*)(void* skeleton, std::uint32_t layer, void* modifier[2],
                                           const char* name);
CreateReins create_reins = nullptr;
BuildReins build_reins = nullptr;

// The modifier's Execute (its vtable's slot 5): (the modifier, the pose params: the
// skeleton's pose at + 8, the horse's world rotation at + 20, its position at + 36). With a driver
// it takes the reins' two ends (pairs of kind 6 and 7) to their hand bones; without one it leaves
// them as the horse's own clip has them, up at its neck. Free reins lie where the driver's hands
// will reach for them, so the climb's clip takes them from there.
using ReinsExecute = char(__fastcall*)(void* modifier, std::uint8_t* params);
ReinsExecute original_reins_execute = nullptr;
constexpr std::size_t kReinsPairSize = 32;
constexpr std::size_t kReinsPairBone = 2;
constexpr std::uint8_t kReinsRightHand = 6;
constexpr std::uint8_t kReinsLeftHand = 7;
constexpr std::size_t kParamsPose = 8;
constexpr std::size_t kParamsTurn = 20;
constexpr std::size_t kParamsPlace = 36;
constexpr std::size_t kPoseRead = 10;
constexpr std::size_t kPoseWrite = 14;

// How long the reins' ends take between where they lie and the hands, when taken and let go.
constexpr double kReinsBlendSeconds = 0.3;

struct ReinsRest {
  void* modifier = nullptr;
  float points[6] = {};
  // Someone is to hold the reins; how far their ends have gone to the hands (0 lying, 1 held), and
  // when that was last worked out.
  bool held = false;
  double blend = 0.0;
  std::chrono::steady_clock::time_point blended_at{};
};
// Written on the engine thread, read on the animation job's.
std::mutex reins_rest_lock;
std::vector<ReinsRest> reins_rests;

// Where the free reins lie and whether someone is to hold them. True while their ends have not all
// come off the hands yet: the modifier keeps its driver until then.
bool keep_reins_rest(void* modifier, const float points[6], bool held) {
  std::lock_guard<std::mutex> guard(reins_rest_lock);
  for (ReinsRest& rest : reins_rests) {
    if (rest.modifier == modifier) {
      std::memcpy(rest.points, points, sizeof(rest.points));
      rest.held = held;
      return rest.blend > 0.0;
    }
  }
  ReinsRest rest;
  rest.modifier = modifier;
  std::memcpy(rest.points, points, sizeof(rest.points));
  rest.held = held;
  rest.blended_at = std::chrono::steady_clock::now();
  reins_rests.push_back(rest);
  return false;
}

void forget_reins_rest(void* modifier) {
  std::lock_guard<std::mutex> guard(reins_rest_lock);
  for (std::size_t i = 0; i < reins_rests.size(); ++i) {
    if (reins_rests[i].modifier == modifier) {
      reins_rests.erase(reins_rests.begin() + std::ptrdiff_t(i));
      return;
    }
  }
}

struct Float3 {
  float x, y, z;
};

Float3 cross(const Float3& a, const Float3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// A vector turned by a rotation (x, y, z, w), or against it with `sign` -1.
Float3 turned_by(const float* q, const Float3& v, float sign) {
  const Float3 axis{q[0] * sign, q[1] * sign, q[2] * sign};
  const Float3 first = cross(axis, v);
  const Float3 twice{first.x + q[3] * v.x, first.y + q[3] * v.y, first.z + q[3] * v.z};
  const Float3 second = cross(axis, twice);
  return {v.x + 2.0f * second.x, v.y + 2.0f * second.y, v.z + 2.0f * second.z};
}

char __fastcall hooked_reins_execute(void* modifier, std::uint8_t* params) {
  const char done = original_reins_execute(modifier, params);
  if (!done) {
    return done;
  }
  auto* raw = static_cast<std::uint8_t*>(modifier);
  ReinsRest rest;
  const bool driven = *reinterpret_cast<const std::uint32_t*>(raw + kReinsDriver) != 0;
  {
    std::lock_guard<std::mutex> guard(reins_rest_lock);
    ReinsRest* found = nullptr;
    for (ReinsRest& known : reins_rests) {
      if (known.modifier == modifier) {
        found = &known;
      }
    }
    if (!found) {
      return done;
    }
    const auto now = std::chrono::steady_clock::now();
    const double step =
        std::chrono::duration<double>(now - found->blended_at).count() / kReinsBlendSeconds;
    found->blended_at = now;
    found->blend = std::clamp(found->blend + (found->held ? step : -step), 0.0, 1.0);
    rest = *found;
  }
  if (driven && rest.blend >= 1.0) {
    return done;
  }
  const float share = driven ? float(rest.blend * rest.blend * (3.0 - 2.0 * rest.blend)) : 0.0f;
  void* pose = *reinterpret_cast<void**>(params + kParamsPose);
  const auto* turn = reinterpret_cast<const float*>(params + kParamsTurn);
  const auto* place = reinterpret_cast<const float*>(params + kParamsPlace);
  auto* pair = *reinterpret_cast<std::uint8_t**>(raw + kReinsPairsBegin);
  auto* const end = *reinterpret_cast<std::uint8_t**>(raw + kReinsPairsEnd);
  using ReadBone = const float*(__fastcall*)(void* pose, unsigned bone);
  using WriteBone = void*(__fastcall*)(void* pose, unsigned bone, const float* place_and_turn);
  for (; pair != end; pair += kReinsPairSize) {
    if (pair[0] != kReinsRightHand && pair[0] != kReinsLeftHand) {
      continue;
    }
    const auto bone = unsigned(*reinterpret_cast<const std::int16_t*>(pair + kReinsPairBone));
    const float* now = slot<ReadBone>(pose, kPoseRead)(pose, bone);
    const float* point = pair[0] == kReinsRightHand ? rest.points : rest.points + 3;
    const Float3 local =
        turned_by(turn, {point[0] - place[0], point[1] - place[1], point[2] - place[2]}, -1.0f);
    const float placed[7] = {now[0],
                             now[1],
                             now[2],
                             now[3],
                             local.x + (now[4] - local.x) * share,
                             local.y + (now[5] - local.y) * share,
                             local.z + (now[6] - local.z) * share};
    slot<WriteBone>(pose, kPoseWrite)(pose, bone, placed);
  }
  return done;
}

// Lets go of a std::shared_ptr's count block (the uses at + 8, the weak at + 12; its vtable's
// _Destroy and _Delete_this), as std::_Ref_count_base::_Decref does.
void release_count(void* count) {
  if (!count) {
    return;
  }
  auto* block = static_cast<std::uint8_t*>(count);
  void** vtable = *reinterpret_cast<void***>(block);
  if (_InterlockedDecrement(reinterpret_cast<volatile long*>(block + 8)) == 0) {
    reinterpret_cast<void(__fastcall*)(void*)>(vtable[0])(block);
    if (_InterlockedDecrement(reinterpret_cast<volatile long*>(block + 12)) == 0) {
      reinterpret_cast<void(__fastcall*)(void*)>(vtable[1])(block);
    }
  }
}

struct CartHorse {
  std::uint32_t engine_id = 0;
  void* actor = nullptr;
  void* animated = nullptr;
  void* skeleton = nullptr;
  Gait gait = Gait::Stand;
  // The reins' step, made for this horse and cart (null until the cart is there).
  std::shared_ptr<ReinsStep> reins;
  // When the horse's use prompts are switched off again.
  double prompts_off_at = 0.0;
  // The place it was last given (and whether it was given one yet).
  core::Vec3 place{};
  core::Quat turn{};
  bool placed = false;
};

float quat_dot(const core::Quat& a, const core::Quat& b) {
  return std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
}

float apart(const core::Vec3& a, const core::Vec3& b) {
  const float dx = a.x - b.x;
  const float dy = a.y - b.y;
  const float dz = a.z - b.z;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Puts the horse at its place: every frame while the place moves (the cart goes), and while it
// stays only when the horse drifted from it.
void hold_at(CartHorse& horse, void* entity, const core::Vec3& p, const core::Quat& q) {
  const bool stays = horse.placed && apart(horse.place, p) < kPlaceStays &&
                     quat_dot(horse.turn, q) > kHoldTurnSlack;
  horse.place = p;
  horse.turn = q;
  horse.placed = true;
  if (stays) {
    const float* m = slot<const float*(__fastcall*)(void*)>(entity, kWorldTmSlot)(entity);
    const core::Vec3 at{m[3], m[7], m[11]};
    if (apart(at, p) < kHoldSlack && quat_dot(rotation(m), q) > kHoldTurnSlack) {
      return;
    }
  }
  set_transform(entity, p, q);
}

void drop_reins(CartHorse& horse) {
  if (horse.reins) {
    forget_reins_rest(horse.reins->modifier);
    release_count(horse.reins->modifier_count);
    horse.reins.reset();
  }
}

// The engine id of a player's body here: the local player's or their replica's (0 for none).
std::uint32_t body_engine_id(std::uint32_t player) {
  if (player == 0) {
    return 0;
  }
  if (player == std::uint32_t(local_network_id.load())) {
    return local_engine_id.load();
  }
  const auto found = remotes.find(core::EntityId(player));
  return found != remotes.end() ? found->second.engine_id : 0;
}

// The horse's reins on the cart and in the driver's hands, this frame.
void keep_reins(CartHorse& horse, HarnessSide side, const char* cart_root, std::uint32_t driver,
                const float reins_rest[6]) {
  if (!create_reins || !build_reins || side == HarnessSide::Alone || !cart_root ||
      !horse.skeleton) {
    return;
  }
  void* cart = kcds::runtime::props::entity::find(cart_root);
  if (!cart) {
    drop_reins(horse);
    return;
  }
  if (horse.reins &&
      (horse.reins->cart_entity != cart || horse.reins->horse_actor != horse.actor)) {
    drop_reins(horse);
  }
  if (!horse.reins) {
    void* made[2] = {nullptr, nullptr};
    if (!create_reins(nullptr, made) || !made[0]) {
      return;
    }
    horse.reins = std::make_shared<ReinsStep>();
    horse.reins->horse_actor = horse.actor;
    horse.reins->side = side == HarnessSide::Left ? 0 : 1;
    horse.reins->cart_entity = cart;
    horse.reins->modifier = made[0];
    horse.reins->modifier_count = made[1];
  }
  // As the game's step does: the bone pairs of its side the first time, and the
  // driver; push_cart_reins puts it on the skeleton before the next update.
  auto* modifier = static_cast<std::uint8_t*>(horse.reins->modifier);
  void* const* pairs = reinterpret_cast<void* const*>(modifier + kReinsPairsBegin);
  if (pairs[0] == *reinterpret_cast<void* const*>(modifier + kReinsPairsEnd)) {
    build_reins(horse.reins.get(), char(horse.reins->side));
  }
  const std::uint32_t holder = body_engine_id(driver);
  const bool leaving_hands = keep_reins_rest(horse.reins->modifier, reins_rest, holder != 0);
  // The last holder keeps them until their ends have come off the hands.
  if (holder != 0 || !leaving_hands) {
    *reinterpret_cast<std::uint32_t*>(modifier + kReinsDriver) = holder;
  }
}

// Engine thread only: entity name -> horse.
std::unordered_map<std::string, CartHorse> horses;
// Names whose spawn was refused -> when to try again.
std::unordered_map<std::string, double> retry_at;

void* actor_system_now() {
  void* framework = framework_slot ? *framework_slot : nullptr;
  if (!framework) {
    return nullptr;
  }
  // CCryAction::GetIActorSystem (vtable 25) returns this+1304 (core/frame.cpp).
  return *reinterpret_cast<void**>(static_cast<std::uint8_t*>(framework) + 1304);
}

void* entity_system_of(void* actor_system) {
  return *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor_system) + 48);
}

void forget_inputs(CartHorse& horse) {
  drop_reins(horse);
  kcds::runtime::appearance::outfit::forget(kOutfitKey | horse.engine_id);
  if (!horse.skeleton) {
    return;
  }
  std::lock_guard<std::shared_mutex> lock(motion_mutex);
  remote_inputs.erase(horse.skeleton);
}

// The horse's skeleton as its body has it now (building a body anew gives a new one).
void* skeleton_of(void* entity) {
  void* character = slot<void*(__fastcall*)(void*, unsigned)>(entity, 101)(entity, 0);
  return character ? slot<void*(__fastcall*)(void*)>(character, 5)(character) : nullptr;
}

CartHorse* spawn(const std::string& name, void* actor_system, const core::Vec3& p,
                 const core::Quat& q, double now) {
  const auto retry = retry_at.find(name);
  if (retry != retry_at.end() && now < retry->second) {
    return nullptr;
  }
  using CreateActor =
      void*(__fastcall*)(void*, unsigned short, const char*, const char*, const core::Vec3*,
                         const core::Quat*, const core::Vec3*, std::uint32_t);
  const core::Vec3 scale{1, 1, 1};
  void* actor = nullptr;
  {
    kcds::runtime::npcs::OwnSpawn own;
    actor = slot<CreateActor>(actor_system, 5)(actor_system, 0, name.c_str(), "Horse", &p, &q,
                                               &scale, 0);
  }
  if (!actor || (*reinterpret_cast<void***>(actor))[7] != native_horse_init) {
    log("CART HORSE %s refused by the game", name.c_str());
    retry_at[name] = now + kRetrySeconds;
    return nullptr;
  }
  void* entity = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + 56);
  if (!entity) {
    retry_at[name] = now + kRetrySeconds;
    return nullptr;
  }
  retry_at.erase(name);
  CartHorse horse;
  horse.engine_id = *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(entity) + 12);
  horse.actor = actor;
  horse.animated = slot<void*(__fastcall*)(void*)>(actor, 90)(actor);
  log("CART HORSE %s spawned engine=0x%x", name.c_str(), horse.engine_id);
  return &(horses[name] = horse);
}

}  // namespace

bool keep_cart_horse(const char* name, const float position[3], const float rotation[4],
                     const float velocity[3], float turn_rate, HarnessSide side,
                     const char* cart_root, std::uint32_t driver, const float reins_rest[6]) {
  void* actor_system = actor_system_now();
  if (!actor_system) {
    return false;
  }
  void* entity_system = entity_system_of(actor_system);
  const core::Vec3 p{position[0], position[1], position[2]};
  const core::Quat q{rotation[0], rotation[1], rotation[2], rotation[3]};
  CartHorse* horse = nullptr;
  const auto found = horses.find(name);
  if (found != horses.end()) {
    if (get_entity(entity_system, found->second.engine_id)) {
      horse = &found->second;
    } else {
      // The game removed the body itself.
      forget_inputs(found->second);
      horses.erase(found);
    }
  }
  if (!horse) {
    horse = spawn(name, actor_system, p, q, local::clock_seconds());
  }
  if (!horse) {
    return false;
  }
  void* entity = get_entity(entity_system, horse->engine_id);
  if (!entity) {
    return false;
  }
  hold_at(*horse, entity, p, q);
  // Nobody mounts or inspects a harnessed horse: the horse script's own switches for its use
  // prompts (Scripts/Entities/AI/Horse.lua GetActions), set again now and then as the script may
  // set them anew.
  const double now = local::clock_seconds();
  if (now >= horse->prompts_off_at) {
    horse->prompts_off_at = now + kPromptsOffSeconds;
    kcds::runtime::props::entity::set_script_flag(entity, "mountableByPlayer", false);
    kcds::runtime::props::entity::set_script_flag(entity, "inspectableByPlayer", false);
  }
  kcds::runtime::appearance::outfit::apply(kOutfitKey | horse->engine_id, horse->actor, entity,
                                           harness_outfit(side));
  void* skeleton = skeleton_of(entity);
  if (skeleton != horse->skeleton) {
    forget_inputs(*horse);
    horse->skeleton = skeleton;
    horse->gait = Gait::Stand;
  }
  if (!horse->animated) {
    horse->animated = slot<void*(__fastcall*)(void*)>(horse->actor, 90)(horse->actor);
  }
  // Its gait from the cart's speed along its heading (the rotation's forward, y, axis).
  const float speed = std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1]);
  const float forward_x = 2.0f * (q.x * q.y - q.w * q.z);
  const float forward_y = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
  const float ahead = velocity[0] * forward_x + velocity[1] * forward_y;
  Gait gait = Gait::Stand;
  if (speed >= kStandingSpeed) {
    if (ahead < 0.0f) {
      gait = Gait::Back;
    } else {
      gait = speed > kWalkingSpeed ? Gait::Trot : Gait::Walk;
    }
  }
  if (gait != horse->gait) {
    // A clip the skeleton refused this frame is asked for again the next one (it can refuse
    // while the last one still blends out); the gait counts as changed once it plays.
    const char* clip = clip_of(gait);
    bool started = true;
    if (clip) {
      started = kcds::runtime::npcs::play_clip(entity, clip, true);
    } else {
      kcds::runtime::npcs::stop_clip(entity);
    }
    if (started) {
      log("CART HORSE %s gait %s", name, clip ? clip : "stand");
      horse->gait = gait;
    }
  }
  RemoteInputs walk;
  walk.animated = horse->animated;
  walk.valid = true;
  walk.motion.type = kVelocityRequest;
  walk.motion.velocity = {velocity[0], velocity[1], velocity[2]};
  // The request's rotation is a turn from the current heading; the heading is set above.
  walk.motion.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
  walk.mask = (1u << kMoveSpeedParam) | (1u << kTurnSpeedParam);
  walk.params[kMoveSpeedParam] = speed;
  walk.params[kTurnSpeedParam] = turn_rate;
  if (skeleton) {
    {
      std::lock_guard<std::shared_mutex> lock(motion_mutex);
      remote_inputs.assign(skeleton, walk);
    }
    original_motion_param(skeleton, kMoveSpeedParam, speed);
    original_motion_param(skeleton, kTurnSpeedParam, turn_rate);
  }
  if (horse->animated) {
    original_movement_request(horse->animated, &walk.motion);
  }
  keep_reins(*horse, side, cart_root, driver, reins_rest);
  return true;
}

void push_cart_reins() {
  if (horses.empty()) {
    return;
  }
  void* actor_system = actor_system_now();
  void* entity_system = actor_system ? entity_system_of(actor_system) : nullptr;
  if (!entity_system) {
    return;
  }
  for (auto& [name, horse] : horses) {
    if (!horse.reins || !get_entity(entity_system, horse.engine_id)) {
      continue;
    }
    void* skeleton = skeleton_of(get_entity(entity_system, horse.engine_id));
    if (!skeleton || skeleton != horse.skeleton) {
      continue;
    }
    // A counted copy of the modifier's shared_ptr, which the skeleton keeps until it ran.
    void* count = horse.reins->modifier_count;
    if (count) {
      _InterlockedIncrement(
          reinterpret_cast<volatile long*>(static_cast<std::uint8_t*>(count) + 8));
    }
    void* copy[2] = {horse.reins->modifier, count};
    slot<PushPoseModifier>(skeleton, kPushPoseModifierSlot)(skeleton, kReinsLayer, copy,
                                                            "C_CartHorseReinsModifier");
  }
}

bool install_cart_reins() {
  create_reins = reinterpret_cast<CreateReins>(resolve_any(
      "CartReinsModifierCreate",
      {{"48 89 5C 24 08 57 48 83 EC 40 48 C7 02 00 00 00 00 48 8B FA 48 8B 4A 08 48 C7 42 08 00 00 "
        "00 00 48 85 C9 74 05 E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 98 05 00 00 48 85 "
        "C0 0F 84 A2 00 00 00 48 8B 08 48 8D 15 43 04 E2 01"},
       // The Microsoft Store build.
       {"48 89 5C 24 08 57 48 83 EC 40 48 C7 02 00 00 00 00 48 8B FA 48 8B 4A 08 48 C7 42 08 00 00 "
        "00 00 48 85 C9 74 05 E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 98 05 00 00 48 85 "
        "C0 0F 84 A2 00 00 00 48 8B 08 48 8D 15 ?? ?? ?? 01 4C 8B 41 08 48 8B C8 41 FF D0 48 8B D8 "
        "48 85 C0 0F 84 82 00 00 00 48 8B 08 48 8D 15 7B 32 7A 02"}}));
  build_reins = reinterpret_cast<BuildReins>(resolve_unique(
      "CartReinsModifierBuild",
      "48 89 5C 24 10 48 89 74 24 18 55 57 41 56 48 8B EC 48 83 EC 70 40 8A F2 0F 29 74 24 60 48 "
      "8B F9 48 C7 45 C0 00 00 00 00 0F 57 C0 48 C7 45 20 04 00 00 00"));
  void* reins_execute = resolve_unique("CartReinsModifierExecute",
                                       "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 56 41 "
                                       "57 48 8D A8 38 FF FF FF 48 81 EC B0 "
                                       "01 00 00 0F 29 70 D8 48 8B F9 48 8B 0D ?? ?? ?? ?? 4C 8B "
                                       "F2 0F 29 78 C8 44 0F 29 40 B8 8B 57 "
                                       "24");
  if (reins_execute) {
    const bool hooked = MH_CreateHook(reins_execute, reinterpret_cast<void*>(&hooked_reins_execute),
                                      reinterpret_cast<void**>(&original_reins_execute)) == MH_OK &&
                        MH_EnableHook(reins_execute) == MH_OK;
    log("CARTS free reins on the plank %s", hooked ? "installed" : "unavailable");
  }
  return create_reins && build_reins;
}

void remove_cart_horse(const char* name) {
  const auto found = horses.find(name);
  retry_at.erase(name);
  if (found == horses.end()) {
    return;
  }
  forget_inputs(found->second);
  void* actor_system = actor_system_now();
  void* entity_system = actor_system ? entity_system_of(actor_system) : nullptr;
  if (entity_system && get_entity(entity_system, found->second.engine_id)) {
    slot<void(__fastcall*)(void*, std::uint32_t, bool)>(entity_system, 19)(
        entity_system, found->second.engine_id, false);
  }
  horses.erase(found);
}

bool cart_horse_actor(void* actor) {
  if (!actor) {
    return false;
  }
  for (const auto& [name, horse] : horses) {
    if (horse.actor == actor) {
      return true;
    }
  }
  return false;
}

void reset_cart_horses() {
  for (auto& [name, horse] : horses) {
    forget_inputs(horse);
  }
  horses.clear();
  retry_at.clear();
}

}  // namespace kcds::game_client
