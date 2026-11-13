// Horses in the world (see native_horses.hpp). Engine update thread only.
// MCP: CActorCreator<C_Horse> -> C_Horse primary table -> v7 C_Horse::Init.
// Scripts.pak Horse.lua supplies the original model, animation DB and generic soul.
#include "../npcs/native_npcs.hpp"
#include "native_horses.hpp"
#include "../replicas/character_retirement.hpp"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cmath>
#include <mutex>

#include "../../../Backend/integration/local/rider_state.hpp"
#include "../carts/cart_horses.hpp"
#include "../core/game_layouts.hpp"
#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../knockout/knockout.hpp"
#include "../appearance/grime.hpp"
#include "horse_speed_scale.hpp"
#include "../appearance/outfit.hpp"
#include "../buffs/horse_buffs.hpp"
#include "../inventory/item_names.hpp"
#include "kcd2s/integration/hands.hpp"
#include "../props/prop_entity.hpp"
#include "../npcs/npc_body.hpp"
#include "../npcs/npc_health.hpp"
#include "../npcs/npc_suppression.hpp"
#include "../npcs/npc_walk.hpp"
#include "../replicas/motion_capture.hpp"
#include "../replicas/remote.hpp"
#include "MinHook.h"
#include "horse_capacity.hpp"
#include "horse_tack.hpp"
#include "player_horse.hpp"

using namespace kcd2s;

namespace kcds::game_client {

HorseMount original_horse_mount, native_remote_horse_mount;
HorseDismount original_horse_dismount;
RemoteDismount native_remote_horse_dismount;
HumanMounted native_human_mounted;
core::EntityId local_rider_horse{};
std::uint64_t local_rider_generation{};
core::EntityId requested_local_horse{};
bool requested_local_mount{};
double local_mount_started{}, next_mount_report{};
bool observed_local_mount{};
std::unordered_map<core::EntityId, NativeHorse> native_horses;
std::unordered_map<core::EntityId, double> horse_retry_at;
// The horse the server gives the local player is its own horse in this game, not a stolen one
// (riding/owned_horse.hpp chooses it, riding/player_horse.hpp tells the game).
kcds::runtime::riding::OwnedHorse owned_horse;
bool player_horses_installed = false;

namespace {
// A replica's dismount the game just began, ended at once: its rider action (the helper at
// actor + 640, its getter's action 43) detaches and completes, as the owner's completion does
// (native_horse_completion). False when the action is not the game's dismount.
bool finish_replica_dismount(void* actor) {
  constexpr std::size_t kActionHelper = 640;
  constexpr unsigned char kDismountAction = 43;
  constexpr std::size_t kHelperGetterSlot = 8;
  constexpr std::size_t kActionEventSlot = 53;
  constexpr std::size_t kActionReleaseSlot = 2;
  auto helper = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + kActionHelper);
  if (!helper || (*reinterpret_cast<void***>(helper))[kHelperGetterSlot] !=
                     reinterpret_cast<void*>(native_rider_action_getter)) {
    return false;
  }
  void* action = nullptr;
  native_rider_action_getter(helper, &action, kDismountAction);
  if (!action) {
    return false;
  }
  const bool dismount = (*reinterpret_cast<void***>(action))[kActionEventSlot] ==
                        reinterpret_cast<void*>(native_dismount_completed);
  if (dismount) {
    native_dismount_detach(action);
    native_dismount_completed(action);
  }
  slot<void(__fastcall*)(void*)>(action, kActionReleaseSlot)(action);
  return dismount;
}

void detach_replica_rider(core::EntityId id, NativeHorse& horse) {
  if (!horse.replica_rider) {
    return;
  }
  const auto old = remotes.find(horse.replica_rider);
  if (old != remotes.end()) {
    native_remote_horse_dismount(old->second.actor, 0, 1);
    // The owner already left the saddle in their game (a horse that threw its rider,
    // Horse.buckRider, left the replica sliding off through the game's whole dismount for 3 s,
    // then lying where the owner fell): it leaves at once and shows the owner's
    // own fall.
    const bool finished = finish_replica_dismount(old->second.actor);
    log("HORSE replica native dismount before retirement horse=%u rider=%u finished=%u", id,
        horse.replica_rider, unsigned(finished));
  }
  horse.replica_rider = 0;
  horse.next_mount_attempt = 0;
}

// A horse another game simulates plays its simulator's base-layer clip on its simulator's time
// The clips start together, but in the replica's game the clip's time drifted and jumped
// back mid-clip with no start, setter or parameter change of its own (live trace: 8
// jumps in 3.5 min, the owner's clip never did), and the head was then off where the owner's was
// not. The simulator sends its clip's time (the whole-clip progress getter) with every state; the
// replica's clip takes it (C_SkeletonAnim::SetAnimationNormalizedTime, slot 27, as
// ladder_clips.cpp) once it is more than kHorseClipDrift off for kHorseClipDriftMs. The
// simulator's own clip time moves in steps while its game does not show the horse (0.06 to
// 0.09 at a time), and a replica that followed every step went back and forth several times a
// second; its own jumps stay off until the next clip.
constexpr float kHorseClipDrift = 0.15f;
constexpr ULONGLONG kHorseClipDriftMs = 750;
std::unordered_map<core::EntityId, ULONGLONG> horse_clip_drift_since;
constexpr std::size_t kSetAnimationNormalizedTime = 27;
constexpr std::size_t kAnimationProgressSlot = 28;
constexpr std::uint32_t kAnimationLoop = 2;

void keep_clip_on_simulator_time(core::EntityId id, void* skeleton,
                                 const local::AnimationLayer& owner) {
  if (!skeleton || owner.clip < 0 ||
      (*reinterpret_cast<void***>(skeleton))[kAnimationProgressSlot] !=
          reinterpret_cast<void*>(native_animation_progress)) {
    return;
  }
  const auto count = slot<unsigned(__fastcall*)(void*, unsigned)>(skeleton, 15)(skeleton, 0);
  if (count == 0 || count > 16) {
    return;
  }
  auto raw = slot<std::uint8_t*(__fastcall*)(void*, unsigned, unsigned)>(skeleton, 18)(skeleton, 0,
                                                                                       count - 1);
  if (!raw || *reinterpret_cast<const int*>(raw + 16) != owner.clip) {
    return;
  }
  const float progress = native_animation_progress(skeleton, raw);
  float drift = owner.params.normalized_time - progress;
  // A looping clip is as near the other way round.
  if (owner.params.flags & kAnimationLoop) {
    drift = std::remainder(drift, 1.0f);
  }
  const ULONGLONG now = GetTickCount64();
  if (std::fabs(drift) <= kHorseClipDrift) {
    horse_clip_drift_since.erase(id);
    return;
  }
  const auto since = horse_clip_drift_since.emplace(id, now).first->second;
  if (now - since < kHorseClipDriftMs) {
    return;
  }
  horse_clip_drift_since.erase(id);
  slot<void(__fastcall*)(void*, void*, float, bool)>(skeleton, kSetAnimationNormalizedTime)(
      skeleton, raw, owner.params.normalized_time, true);
  log("HORSE clip on its simulator's time horse=%u clip=%d %.3f -> %.3f", id, owner.clip, progress,
      owner.params.normalized_time);
}
}  // namespace

// Before a horse's body leaves this game: when the game holds it as the local player's horse, it
// lets it go first.
void release_owned_horse(core::EntityId id, const NativeHorse& horse) {
  if (!player_horses_installed) {
    return;
  }
  if (kcds::runtime::riding::release_player_horse(local_combat_actor.load(), horse.actor)) {
    log("HORSE player horse let go network=%u: its body leaves", id);
  }
  owned_horse.forget(id);
}

// After this frame's offers (horses the server gives the local player, with a ready body): make
// the chosen one the player's horse, or let the game's go when none is given any more.
void apply_owned_horse(void* local_actor) {
  using Step = kcds::runtime::riding::OwnedHorse::Step;
  const Step step = owned_horse.decide();
  if (step == Step::Keep) {
    return;
  }
  if (step == Step::Clear) {
    log("HORSE player horse let go network=%u: no longer given", owned_horse.held());
    kcds::runtime::riding::clear_player_horse(local_actor);
    owned_horse.cleared();
    return;
  }
  const auto id = owned_horse.wanted();
  const auto it = native_horses.find(id);
  if (it == native_horses.end()) {
    return;
  }
  const bool accepted =
      kcds::runtime::riding::make_player_horse(local_actor, it->second.actor, it->second.engine_id);
  owned_horse.made(id, accepted);
  log("HORSE player horse network=%u engine=0x%x accepted=%u", id, it->second.engine_id,
      unsigned(accepted));
}
char __fastcall hooked_horse_mount(void* actor, void* horse, char instant) {
  // A cart's horse is harnessed: nobody rides it away (carts/cart_horses.hpp).
  if (cart_horse_actor(horse)) {
    return 0;
  }
  auto result = original_horse_mount(actor, horse, instant);
  if (result && actor == local_combat_actor.load()) {
    for (const auto& [id, h] : native_horses)
      if (h.actor == horse) {
        local_rider_horse = id;
        ++local_rider_generation;
        requested_local_horse = id;
        requested_local_mount = true;
        observed_local_mount = false;
        local_mount_started = local::clock_seconds();
        next_mount_report = 0;
        log("HORSE local native mount accepted horse=%u tick=%llu", id, GetTickCount64());
        mount_trace_until.store(GetTickCount64() + 4000);
        break;
      }
  }
  return result;
}
bool mount_ordered_horse(core::EntityId horse, bool instant) {
  // How near the player must stand: the game's own get-on starts beside the horse.
  constexpr float kMountOrderReach = 6.0f;
  void* const actor = local_combat_actor.load();
  const auto found = native_horses.find(horse);
  if (!actor || found == native_horses.end() || !found->second.actor) {
    log("HORSE mount order horse=%u: no body here", unsigned(horse));
    return false;
  }
  void* rider = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + 56);
  void* body = kcds::runtime::props::entity::by_id(found->second.engine_id);
  float from[3];
  float to[3];
  if (!rider || !body) {
    return false;
  }
  kcds::runtime::props::entity::world_position(rider, from);
  kcds::runtime::props::entity::world_position(body, to);
  const float dx = to[0] - from[0];
  const float dy = to[1] - from[1];
  if (dx * dx + dy * dy > kMountOrderReach * kMountOrderReach) {
    log("HORSE mount order horse=%u: %.1f m away", unsigned(horse), std::sqrt(dx * dx + dy * dy));
    return false;
  }
  const char mounted = hooked_horse_mount(actor, found->second.actor, instant ? 1 : 0);
  log("HORSE mount order horse=%u instant=%u %s", unsigned(horse), unsigned(instant),
      mounted ? "mounts" : "refused by the game");
  return mounted != 0;
}

std::int64_t __fastcall hooked_horse_dismount(void* actor, std::uint64_t a2, std::uint64_t a3);

bool dismount_ordered() {
  void* const actor = local_combat_actor.load();
  if (!actor || !local_rider_seated.load()) {
    log("HORSE dismount order: not riding");
    return false;
  }
  hooked_horse_dismount(actor, 1, 0);
  log("HORSE dismount order: gets off");
  return true;
}

// A rider pulled down leaves the saddle in their own game as the attacker's lunge reaches them; the
// other game follows the owner, as with every dismount.
constexpr std::uint64_t kPullDownReachMs = 900;
std::uint64_t pull_down_off_at = 0;

void tick_pull_down() {
  if (pull_down_off_at != 0 && GetTickCount64() >= pull_down_off_at) {
    pull_down_off_at = 0;
    dismount_ordered();
  }
}

bool pull_down_ordered(std::uint32_t attacker, std::uint32_t rider) {
  // C_ScriptBindActor::RequestHorsePullDown: the attacker's combat object (actor +
  // 632), what its slot 79 gives, whose slot 2 takes the rider's entity id.
  constexpr std::size_t kCombat = 632;
  constexpr std::size_t kPullDownSlot = 79;
  constexpr std::size_t kRequestSlot = 2;
  void* attacker_actor = nullptr;
  void* rider_actor = nullptr;
  engine_body_of(attacker, attacker_actor);
  const std::uint32_t rider_entity = engine_body_of(rider, rider_actor);
  if (local_rider_seated.load() && (!rider_actor || rider_actor == local_combat_actor.load())) {
    pull_down_off_at = GetTickCount64() + kPullDownReachMs;
  }
  void* combat =
      attacker_actor
          ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(attacker_actor) + kCombat)
          : nullptr;
  if (!combat || !rider_entity) {
    log("HORSE pull down %u on %u: no combat object or body here", attacker, rider);
    return false;
  }
  using Get = void*(__fastcall*)(void*);
  using Request = void(__fastcall*)(void*, std::uint32_t);
  void* requests = (*static_cast<Get**>(combat))[kPullDownSlot](combat);
  (*static_cast<Request**>(requests))[kRequestSlot](requests, rider_entity);
  log("HORSE pull down %u on %u (body %u) asked", attacker, rider, rider_entity);
  return true;
}

bool rear_ordered() {
  // C_ScriptBindHorse::RearAndThrowDown: a flag in the horse's rider data, which the
  // game's horse then acts on.
  constexpr std::size_t kRiderData = 2536;
  constexpr std::size_t kRearAndThrow = 266;
  const auto found = native_horses.find(requested_local_horse);
  void* const actor = found == native_horses.end() ? nullptr : found->second.actor;
  auto* data =
      actor ? *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(actor) + kRiderData)
            : nullptr;
  if (!data || !local_rider_seated.load()) {
    log("HORSE rear order: not riding");
    return false;
  }
  data[kRearAndThrow] = 1;
  log("HORSE rear order: horse=%u rears", unsigned(requested_local_horse));
  return true;
}

// Whether the game's dismount can run on this body: it goes to the rider part (actor + 3432), its
// owner (+ 32) and that one's soul (+ 1640), with no check. A replica of a rider has no
// soul: a pull down on the attacker's game asks for it there and crashed the game.
bool can_dismount(void* actor) {
  constexpr std::size_t kRiderPart = 3432;
  constexpr std::size_t kOwner = 32;
  constexpr std::size_t kSoul = 1640;
  auto read = [](void* from, std::size_t offset) -> void* {
    return from ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(from) + offset) : nullptr;
  };
  return read(read(read(actor, kRiderPart), kOwner), kSoul) != nullptr;
}

std::int64_t __fastcall hooked_horse_dismount(void* actor, std::uint64_t a2, std::uint64_t a3) {
  if (!can_dismount(actor)) {
    log("HORSE dismount of a body without a soul: left to its owner's game");
    return 0;
  }
  auto result = original_horse_dismount(actor, a2, a3);
  if (actor == local_combat_actor.load() && requested_local_horse) {
    requested_local_mount = false;
    next_mount_report = 0;
    log("HORSE local native dismount horse=%u tick=%llu", requested_local_horse, GetTickCount64());
  }
  return result;
}
void* native_horse_init{};

bool simulated_horse_actor(void* actor) {
  for (const auto& [id, horse] : native_horses) {
    if (horse.actor == actor) {
      return horse.authority;
    }
  }
  return false;
}

bool horse_replica_actor(void* actor) {
  for (const auto& [id, horse] : native_horses) {
    if (horse.actor == actor) {
      return !horse.authority;
    }
  }
  return false;
}

void log_named_horse_pieces(core::EntityId id, NativeHorse& horse) {
  // The tack is on once the body wears as many pieces as the field holds ("KO2", then per piece
  // 16 class bytes, a quality byte and two health bytes).
  constexpr std::size_t kTagBytes = 3;
  constexpr std::size_t kPieceBytes = 19;
  const std::size_t pieces =
      horse.gear.size() > kTagBytes ? (horse.gear.size() - kTagBytes) / kPieceBytes : 0;
  const std::vector<void*> worn = kcds::runtime::appearance::outfit::worn(horse.actor);
  if (worn.empty() || worn.size() != pieces) {
    return;
  }
  unsigned named = 0;
  for (void* item : worn) {
    const std::string key = horse_piece_key(item);
    std::string name;
    if (!key.empty() && kcds::runtime::inventory::item_name_of("kcds_nm_h_" + key, name)) {
      log("HORSE piece horse=%u %s shows \"%s\"", unsigned(id), key.c_str(), name.c_str());
      ++named;
    } else {
      log("HORSE piece horse=%u %s: the game's own name", unsigned(id),
          key.empty() ? "(no key)" : key.c_str());
    }
  }
  log("HORSE gear horse=%u worn=%zu named=%u", unsigned(id), worn.size(), named);
  horse.gear_names_logged = true;
}

std::string horse_piece_key(void* item) {
  // An item's class (item + 72) and the class's id, its 16 bytes as the outfit field carries them
  // (class + 8), which the server's keys name in hex.
  constexpr std::size_t kItemClass = kcds::layouts::ItemInstance::kClass;
  constexpr std::size_t kClassId = 8;
  for (const auto& [id, horse] : native_horses) {
    if (!horse.actor) {
      continue;
    }
    const std::vector<void*> worn = kcds::runtime::appearance::outfit::worn(horse.actor);
    if (std::find(worn.begin(), worn.end(), item) == worn.end()) {
      continue;
    }
    const auto* item_class = *reinterpret_cast<const std::uint8_t* const*>(
        static_cast<std::uint8_t*>(item) + kItemClass);
    if (!item_class) {
      return {};
    }
    // The class as scripts write it (the server's classBytesHex is the same text).
    kcd2s::integration::ItemClassId class_id{};
    std::memcpy(class_id.data(), item_class + kClassId, class_id.size());
    return std::to_string(id) + "_" + kcd2s::integration::item_class_script_text(class_id);
  }
  return {};
}
// A horse replica moves and turns with its simulator's replayed movement request; its replicated
// position and heading are forced on it only when it drifted from them. Forcing them every frame
// pulled the replica back to the last network update between two updates: a ridden horse and its
// rider jumped while it ran, and while it stood and its rider turned it.
constexpr float kHorseCorrectionMeters = 1.0f;
constexpr float kStandingHorseCorrectionMeters = 0.25f;
constexpr float kHorseCorrectionRadians = 0.35f;
constexpr float kHorseStandingSpeed = 0.3f;
// The native movement counts as stalled when it moved the replica less than this share of the
// replicated speed since the last frame: the game barely updates a window without focus, and the
// replica then hopped from one correction to the next (alt-tabbed observer). A stalled
// replica glides toward the replicated position every frame (glide_horse).
constexpr float kStalledMovementShare = 0.3f;

// The heading (rotation about the up axis) of an entity matrix and of a rotation: the yaw of their
// forward (y) axis.
float matrix_yaw(const float* m) {
  return std::atan2(-m[1], m[5]);
}

float rotation_yaw(const core::Quat& q) {
  const float fx = 2 * (q.x * q.y - q.w * q.z);
  const float fy = 1 - 2 * (q.x * q.x + q.z * q.z);
  return std::atan2(-fx, fy);
}

float yaw_difference(float a, float b) {
  float d = std::fmod(a - b, 6.2831853f);
  if (d > 3.1415927f) {
    d -= 6.2831853f;
  }
  if (d < -3.1415927f) {
    d += 6.2831853f;
  }
  return std::fabs(d);
}

// A correction does not put the replica on the last replicated position: that position is up to a
// network update old, and a replica that took it frame after frame stepped from one update to the
// next (a galloping horse hopped). It glides toward where the replicated position has run on to
// at the replicated velocity since it arrived (no further than kHorseLeadSeconds), closing this
// share of the gap per second; only a gap past kHorseSnapMeters (a teleport) is jumped.
constexpr float kHorseGlideRate = 12.0f;
constexpr double kHorseLeadSeconds = 0.25;
constexpr float kHorseSnapMeters = 5.0f;
// The fastest a corrected body moves on screen: this share of the horse's pace plus this many
// metres a second.
constexpr float kHorseShownPaceShare = 1.5f;
constexpr float kHorseShownWalk = 0.5f;
// And this share of the gap to its target per second on top, so a gap left behind closes.
constexpr float kHorseCatchUpRate = 2.0f;

// The replica rider on a horse this game placed: the game seats the rider in its own update, which
// ran before this frame's placement of the horse and on some frames left the rider where it was,
// so the rider was drawn off the saddle by up to a metre and caught up the next frame. The rider is
// put on its seat now, every frame: the seat is where the game keeps it in the horse's own axes,
// learnt slowly while it rides (a frame the game left it behind does not count).
constexpr float kSeatLearnShare = 0.05f;
constexpr float kSeatOutlierMetres = 0.3f;

void seat_rider_on_horse(NativeHorse& horse, void* entity, void* rider, core::EntityId rider_id) {
  const float* h = entity ? slot<const float*(__fastcall*)(void*)>(entity, 32)(entity) : nullptr;
  const float* r = rider ? slot<const float*(__fastcall*)(void*)>(rider, 32)(rider) : nullptr;
  if (!h || !r) {
    horse.seat_of = 0;
    return;
  }
  // The rider's offset in the horse's axes (the matrix's columns: right, forward, up).
  const float dx = r[3] - h[3];
  const float dy = r[7] - h[7];
  const float dz = r[11] - h[11];
  const core::Vec3 local{dx * h[0] + dy * h[4] + dz * h[8], dx * h[1] + dy * h[5] + dz * h[9],
                         dx * h[2] + dy * h[6] + dz * h[10]};
  if (horse.seat_of != rider_id) {
    horse.seat_of = rider_id;
    horse.rider_seat = local;
    return;
  }
  const float ox = local.x - horse.rider_seat.x;
  const float oy = local.y - horse.rider_seat.y;
  const float oz = local.z - horse.rider_seat.z;
  if (ox * ox + oy * oy + oz * oz < kSeatOutlierMetres * kSeatOutlierMetres) {
    horse.rider_seat.x += ox * kSeatLearnShare;
    horse.rider_seat.y += oy * kSeatLearnShare;
    horse.rider_seat.z += oz * kSeatLearnShare;
  }
  const core::Vec3& s = horse.rider_seat;
  const core::Vec3 at{h[3] + s.x * h[0] + s.y * h[1] + s.z * h[2],
                      h[7] + s.x * h[4] + s.y * h[5] + s.z * h[6],
                      h[11] + s.x * h[8] + s.y * h[9] + s.z * h[10]};
  set_transform(rider, at, kcds::game_client::rotation(r));
}

void glide_horse(NativeHorse& horse, void* entity, void* rider, const float* tm,
                 const core::Vec3& p, const core::Quat& q, const core::Vec3& velocity, double now,
                 double elapsed) {
  // Test hook (KCDS_TEST_HORSE_SNAP=1): the replicated position taken as it is, as before, to
  // compare.
  static const bool snap = GetEnvironmentVariableW(L"KCDS_TEST_HORSE_SNAP", nullptr, 0) > 0;
  const core::Vec3 native{tm[3], tm[7], tm[11]};
  if (snap) {
    set_transform(entity, p, q);
    horse.followed_position = p;
    return;
  }
  const double lead =
      horse.received_at >= 0 ? (std::min)(now - horse.received_at, kHorseLeadSeconds) : 0.0;
  const core::Vec3 ahead{p.x + velocity.x * float(lead), p.y + velocity.y * float(lead),
                         p.z + velocity.z * float(lead)};
  const float gap = std::sqrt((ahead.x - native.x) * (ahead.x - native.x) +
                              (ahead.y - native.y) * (ahead.y - native.y) +
                              (ahead.z - native.z) * (ahead.z - native.z));
  if (gap > kHorseSnapMeters || elapsed <= 0) {
    set_transform(entity, ahead, q);
    horse.followed_position = ahead;
    return;
  }
  const float share = 1.0f - std::exp(-kHorseGlideRate * float(elapsed));
  core::Vec3 at{native.x + (ahead.x - native.x) * share, native.y + (ahead.y - native.y) * share,
                native.z + (ahead.z - native.z) * share};
  // From where it was shown last frame the body moves no faster than a share above its own pace
  // (and a walking pace beside): the correction added to the replayed movement in full read as a
  // small jump on the frames it ran.
  if (horse.placed_known) {
    // Its pace: the replicated velocity, or how fast its replicated position really moves when
    // that is more (the velocity is the gait's, without a server's speed scale). A gap left
    // behind is closed at a speed growing with it, so the body never falls back to a jump.
    const float pace = (std::max)(std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y),
                                  horse.received_speed);
    const float most =
        (kHorseShownPaceShare * pace + kHorseShownWalk + kHorseCatchUpRate * gap) * float(elapsed);
    const float sx = at.x - horse.last_placed.x;
    const float sy = at.y - horse.last_placed.y;
    const float step = std::sqrt(sx * sx + sy * sy);
    if (step > most && step > 0) {
      at.x = horse.last_placed.x + sx * (most / step);
      at.y = horse.last_placed.y + sy * (most / step);
    }
  }
  // The heading the same way: the shorter way round, normalised.
  core::Quat from = kcds::game_client::rotation(tm);
  const float dot = from.w * q.w + from.x * q.x + from.y * q.y + from.z * q.z;
  const float sign = dot < 0 ? -1.0f : 1.0f;
  core::Quat turned;
  turned.x = from.x + (sign * q.x - from.x) * share;
  turned.y = from.y + (sign * q.y - from.y) * share;
  turned.z = from.z + (sign * q.z - from.z) * share;
  turned.w = from.w + (sign * q.w - from.w) * share;
  const float length = std::sqrt(turned.w * turned.w + turned.x * turned.x + turned.y * turned.y +
                                 turned.z * turned.z);
  if (length > 1e-6f) {
    turned.w /= length;
    turned.x /= length;
    turned.y /= length;
    turned.z /= length;
  } else {
    turned = q;
  }
  set_transform(entity, at, turned);
  horse.followed_position = at;
}

// Test hook (KCDS_TEST_REPLICA_TRACE): every 5 s, how a horse replica was placed per frame:
// following its replayed movement, or put on the replicated position (and why).
struct FollowCounts {
  unsigned frames = 0;
  unsigned no_motion = 0;
  unsigned stalled = 0;
  unsigned drifted = 0;
  unsigned turned = 0;
  float largest = 0.0f;
  double since = -1.0;
};

void count_follow(FollowCounts& counts, const char* reason, float metres, double now) {
  static const bool wanted = GetEnvironmentVariableW(L"KCDS_TEST_REPLICA_TRACE", nullptr, 0) > 0;
  if (!wanted) {
    return;
  }
  if (counts.since < 0) {
    counts.since = now;
  }
  ++counts.frames;
  if (reason) {
    const std::string why = reason;
    if (why == "no motion") {
      ++counts.no_motion;
    } else if (why == "stalled") {
      ++counts.stalled;
    } else if (why == "drifted") {
      ++counts.drifted;
    } else {
      ++counts.turned;
    }
    counts.largest = (std::max)(counts.largest, metres);
  }
  if (now - counts.since >= 5.0) {
    log("HORSE FOLLOW frames=%u placed: no_motion=%u stalled=%u drifted=%u turned=%u "
        "largest=%.2f m",
        counts.frames, counts.no_motion, counts.stalled, counts.drifted, counts.turned,
        counts.largest);
    counts = FollowCounts{};
    counts.since = now;
  }
}

// The end of a frame's following: where the replica stands now, for the next frame's correction
// (glide_horse) and, as a test hook (KCDS_TEST_REPLICA_TRACE), in the log to measure how evenly it
// moves.
void finish_follow(NativeHorse& horse, void* entity, void* rider, double now) {
  const float* after = slot<const float*(__fastcall*)(void*)>(entity, 32)(entity);
  if (!after) {
    horse.placed_known = false;
    return;
  }
  horse.last_placed = core::Vec3{after[3], after[7], after[11]};
  horse.placed_known = true;
  if (rider) {
    seat_rider_on_horse(horse, entity, rider, horse.replica_rider);
  } else {
    horse.seat_of = 0;
  }
  static const bool wanted = GetEnvironmentVariableW(L"KCDS_TEST_REPLICA_TRACE", nullptr, 0) > 0;
  if (wanted) {
    const float* seated =
        rider ? slot<const float*(__fastcall*)(void*)>(rider, 32)(rider) : nullptr;
    log("HORSE SHOWN engine=%u now=%.4f at=%.3f,%.3f,%.3f rider=%.3f,%.3f,%.3f", horse.engine_id,
        now, after[3], after[7], after[11], seated ? seated[3] : 0.0f, seated ? seated[7] : 0.0f,
        seated ? seated[11] : 0.0f);
  }
}

void follow_replicated_horse(NativeHorse& horse, void* entity, void* rider, const core::Vec3& p,
                             const core::Quat& q, const core::Vec3* velocity, double now) {
  if (!entity) {
    return;
  }
  static FollowCounts counts;
  const float* placed_from = slot<const float*(__fastcall*)(void*)>(entity, 32)(entity);
  const float jump = placed_from ? std::hypot(p.x - placed_from[3], p.y - placed_from[7]) : 0.0f;
  const float* tm = slot<const float*(__fastcall*)(void*)>(entity, 32)(entity);
  const double elapsed = horse.followed_at > 0 ? now - horse.followed_at : 0;
  horse.followed_at = now;
  if (horse.received_at < 0 || !(p == horse.received_position)) {
    // How fast the replicated position moves, smoothed over a few updates.
    const double since = horse.received_at >= 0 ? now - horse.received_at : 0;
    if (since > 0.01 && since < 1.0) {
      const float moved_by =
          std::hypot(p.x - horse.received_position.x, p.y - horse.received_position.y);
      horse.received_speed = 0.7f * horse.received_speed + 0.3f * moved_by / float(since);
    }
    horse.received_position = p;
    horse.received_at = now;
  }
  if (!tm || !velocity) {
    set_transform(entity, p, q);
    horse.followed_position = p;
    count_follow(counts, "no motion", jump, now);
    finish_follow(horse, entity, rider, now);
    return;
  }
  const core::Vec3 native{tm[3], tm[7], tm[11]};
  const float mx = native.x - horse.followed_position.x;
  const float my = native.y - horse.followed_position.y;
  const float moved = std::sqrt(mx * mx + my * my);
  horse.followed_position = native;
  const float speed_squared =
      velocity->x * velocity->x + velocity->y * velocity->y + velocity->z * velocity->z;
  const bool standing = speed_squared < kHorseStandingSpeed * kHorseStandingSpeed;
  const float expected = std::sqrt(speed_squared) * float(elapsed);
  if (!standing && elapsed > 0 && moved < kStalledMovementShare * expected) {
    glide_horse(horse, entity, rider, tm, p, q, *velocity, now, elapsed);
    count_follow(counts, "stalled", jump, now);
    finish_follow(horse, entity, rider, now);
    return;
  }
  const float limit = standing ? kStandingHorseCorrectionMeters : kHorseCorrectionMeters;
  const float dx = p.x - tm[3];
  const float dy = p.y - tm[7];
  const float dz = p.z - tm[11];
  const bool drifted = dx * dx + dy * dy + dz * dz > limit * limit;
  const bool turned = yaw_difference(matrix_yaw(tm), rotation_yaw(q)) > kHorseCorrectionRadians;
  if (drifted || turned) {
    glide_horse(horse, entity, rider, tm, p, q, standing ? core::Vec3{} : *velocity, now, elapsed);
  }
  count_follow(counts, drifted ? "drifted" : (turned ? "turned" : nullptr), jump, now);
  finish_follow(horse, entity, rider, now);
}
RiderReady native_rider_ready;
RiderActionGetter native_rider_action_getter = nullptr;
RiderActionEvent native_mount_completed = nullptr, native_dismount_completed = nullptr,
                 native_dismount_detach = nullptr;

void* horse_rider_sync(const NativeHorse& horse) {
  auto info = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(horse.actor) + 2536);
  return info ? *reinterpret_cast<void**>(info + 8) : nullptr;
}
std::string capture_native_rider_state(void* actor) {
  if (!local_rider_horse || !local_rider_generation)
    return {};
  auto it = native_horses.find(local_rider_horse);
  if (it == native_horses.end())
    return {};
  auto sync = horse_rider_sync(it->second);
  if (!sync)
    return {};
  if ((*reinterpret_cast<void***>(sync))[12] != reinterpret_cast<void*>(native_rider_ready)) {
    failed = true;
    return {};
  }
  auto state = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(sync) + 32);
  if (!state)
    return {};
  auto rider = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(sync) + 40);
  if (rider && rider != actor)
    return {};
  if (!rider && state[8] != 0)
    return {};
  auto helper = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + 640);
  if (!helper)
    return {};
  if ((*reinterpret_cast<void***>(helper))[8] !=
      reinterpret_cast<void*>(native_rider_action_getter)) {
    failed = true;
    return {};
  }
  std::uint8_t active = 0;
  for (unsigned i = 0; i < 2; ++i) {
    void* action = nullptr;
    native_rider_action_getter(helper, &action, i ? 43 : 41);
    if (action) {
      if (slot<char(__fastcall*)(void*)>(action, 4)(action))
        active |= std::uint8_t(1u << i);
      slot<void(__fastcall*)(void*)>(action, 2)(action);
    }
  }
  return local::encode_rider(
      {local_rider_horse, local_rider_generation, it->second.epoch, state[8], active});
}
// Mirror the original phase callback only. Absence from the helper action lookup
// does not prove completion: Rider owns subordinate actions. Forced v35 Exit
// tears down the mount slave before the riding pose has been established.
void synchronize_native_rider_completion(core::EntityId owner, void* actor,
                                         const std::string& payload) {
  auto owned = local::decode_rider(payload);
  if (!owned)
    return;
  auto it = native_horses.find(owned->horse);
  if (it == native_horses.end())
    return;
  auto& horse = it->second;
  if (owned->epoch != horse.epoch)
    return;
  auto sync = horse_rider_sync(horse);
  if (!sync)
    return;
  if ((*reinterpret_cast<void***>(sync))[12] != reinterpret_cast<void*>(native_rider_ready)) {
    failed = true;
    return;
  }
  auto state = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(sync) + 32);
  if (!state)
    return;
  const auto phase = state[8];
  auto rider = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(sync) + 40);
  // Dismount can clear the native rider before its action tail finishes.
  if (rider != actor && !(rider == nullptr && phase == 3 && horse.completion_owner == owner))
    return;
  if (horse.completion_owner != owner) {
    horse.completion_owner = owner;
    horse.mount_completed_generation = 0;
    horse.dismount_completed_generation = 0;
  }
  const bool mount = owned->phase == 2 && (phase == 1 || phase == 2);
  const bool dismount = owned->phase == 0 && (phase == 3 || phase == 0);
  if (!mount && !dismount)
    return;
  auto& completed = mount ? horse.mount_completed_generation : horse.dismount_completed_generation;
  const bool complete = (mount ? phase == 1 : phase == 3) && owned->generation > completed;
  if (!complete)
    return;
  auto helper = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + 640);
  if (!helper)
    return;
  if ((*reinterpret_cast<void***>(helper))[8] !=
      reinterpret_cast<void*>(native_rider_action_getter)) {
    failed = true;
    return;
  }
  void* action = nullptr;
  native_rider_action_getter(helper, &action, mount ? 41 : 43);
  if (!action)
    return;
  auto event = mount ? native_mount_completed : native_dismount_completed;
  if ((*reinterpret_cast<void***>(action))[53] != reinterpret_cast<void*>(event)) {
    log("HORSE unexpected native action; adapter stopped");
    failed = true;
  } else {
    if (complete) {
      if (dismount)
        native_dismount_detach(action);
      event(action);
      completed = owned->generation;
      log("HORSE owner native completion horse=%u generation=%llu sourcePhase=%u "
          "previousReplicaPhase=%u",
          owned->horse, static_cast<unsigned long long>(owned->generation), unsigned(owned->phase),
          unsigned(phase));
    }
  }
  slot<void(__fastcall*)(void*)>(action, 2)(action);
}
bool horse_rider_transition(const NativeHorse& horse) {
  auto sync = horse_rider_sync(horse);
  if (!sync)
    return false;
  // C_RiderSync primary v12: native riding states 2/10. Its native
  // transition states include mounting 1 and dismounting 3.
  if ((*reinterpret_cast<void***>(sync))[12] != reinterpret_cast<void*>(native_rider_ready)) {
    log("HORSE unexpected RiderSync class; adapter stopped");
    failed = true;
    return true;
  }
  auto state = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(sync) + 32);
  if (!state) {
    failed = true;
    return true;
  }
  const auto phase = state[8];
  // MCP explicitly excludes only mounting(1)/dismounting(3).
  // Ready v12 means riding(2/10), NOT "no transition": detached(0) and
  // other native riding actions must not keep the replica adapter blocked.
  static std::unordered_map<void*, unsigned> phases;
  if (!phases.count(sync) || phases[sync] != phase) {
    phases[sync] = phase;
    log("HORSE native phase horse=%p rider=%p phase=%u transition=%u", horse.actor,
        *reinterpret_cast<void**>(static_cast<std::uint8_t*>(sync) + 40), unsigned(phase),
        unsigned(phase == 1 || phase == 3));
  }
  return phase == 1 || phase == 3;
}
bool native_rider_transition(void* actor) {
  for (const auto& [id, h] : native_horses) {
    (void)id;
    auto sync = horse_rider_sync(h);
    // C_RiderSync v15/v16 return actor at +40 / horse at +48.
    if (sync && *reinterpret_cast<void**>(static_cast<std::uint8_t*>(sync) + 40) == actor)
      return horse_rider_transition(h);
  }
  return false;
}
void clear_native_horses(void* entity_system) {
  // Also when the level unload removes the bodies (no entity system): the game's player horse
  // never names a horse this world had.
  for (const auto& [id, horse] : native_horses) {
    release_owned_horse(id, horse);
  }
  owned_horse.reset();
  for (auto& [id, horse] : native_horses) {
    (void)id;
    if (entity_system && get_entity(entity_system, horse.engine_id)) {
      detach_replica_rider(id, horse);
      record_character_retirement(entity_system, horse.engine_id);
      slot<void(__fastcall*)(void*, std::uint32_t, bool)>(entity_system, 19)(
          entity_system, horse.engine_id, false);
    }
  }
  {
    std::lock_guard<std::shared_mutex> lock(motion_mutex);
    for (const auto& [id, h] : native_horses) {
      (void)id;
      horse_captures.erase(h.skeleton);
      remote_inputs.erase(h.skeleton);
      animation_stop_fences.erase(h.skeleton);
    }
  }
  native_horses.clear();
  horse_retry_at.clear();
  local_rider_horse = 0;
  local_rider_generation = 0;
  requested_local_horse = 0;
  requested_local_mount = false;
}
unsigned ridden_horse_engine(core::EntityId rider) {
  for (const auto& [id, horse] : native_horses) {
    if (horse.replica_rider == rider && horse.engine_id) {
      return horse.engine_id;
    }
  }
  return 0;
}
void* ridden_horse_actor(core::EntityId rider) {
  for (const auto& [id, horse] : native_horses) {
    if (horse.replica_rider == rider && horse.actor) {
      return horse.actor;
    }
  }
  return nullptr;
}

// The horse's health, death and knock-out. The simulator's game decides: it reports
// its body's health and state (HORSE_HEALTH) when they change, and a horse spawned to be knocked
// out holds the game's hidden immortality, so a lethal blow knocks it out. Every game follows
// the server's state: dead bodies die the game's way, knocked-out ones lie down until the server
// wakes them (a new `life`), and replicas show the server's health.
constexpr double kHorseHealthReportSeconds = 0.25;
constexpr double kHorseImmortalitySeconds = 1.0;
constexpr float kHorseHealthReportStep = 0.5f;

// True when the body must be made anew (a new life of a body that lies down or dead).
template <class Fields>
bool tick_horse_health(core::EntityId id, NativeHorse& horse, const Fields& fields, bool authority,
                       double now) {
  namespace health = kcds::runtime::npcs::health;
  namespace knockout = kcds::runtime::knockout;
  if (fields.size() <= integration::HorseLife || !horse.actor) {
    return false;
  }
  void* soul = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(horse.actor) + 1640);
  if (!soul) {
    return false;
  }
  const float server_health = std::get<float>(fields[integration::HorseHealth]);
  const auto server_state = int(std::get<std::int64_t>(fields[integration::HorseCondition]));
  const auto life = std::get<std::int64_t>(fields[integration::HorseLife]);
  const bool knock_out = std::get<bool>(fields[integration::HorseKnockOut]);
  // The server keeps 0 to kHorseMaxHealth; the body's own health is clamped into it.
  const float native_health =
      std::clamp(health::health_of(soul), 0.0f, integration::kHorseMaxHealth);
  const bool down = knockout::creature_unconscious(soul);
  const int native_state = native_health <= 0 ? 2 : down ? 1 : 0;

  // A new life (the resource revived or woke it): a body that lies down or dead is made anew,
  // standing (the game's own end of an unconsciousness does not get a horse up).
  if (life != horse.life) {
    const bool first = horse.life < 0;
    horse.life = life;
    if (!first && server_state == 0 && native_state != 0) {
      log("HORSE health horse=%u gets up (life %lld): body made anew", id, life);
      return true;
    }
  }
  // The server's death and knock-out, on every game's body.
  if (server_state == 2 && native_state != 2) {
    health::kill(soul);
    log("HORSE health horse=%u dies (server)", id);
  } else if (server_state == 1 && native_state == 0) {
    knockout::hold_creature_down(soul);
  }
  // Its current stamina (the soul's state 1, soul + 1800 + 4, player_stats.hpp), as the game that
  // runs the horse has it: reported when it moved by a step, at most kMaxHorseStaminasPerSecond.
  if (authority && now >= horse.next_stamina_report) {
    float stamina = 0;
    std::memcpy(&stamina, static_cast<std::uint8_t*>(soul) + 1804, sizeof(stamina));
    if (std::isfinite(stamina) && stamina >= 0.0f &&
        std::fabs(stamina - horse.sent_stamina) >= integration::kHorseStaminaReportStep) {
      horse.next_stamina_report = now + 1.0 / integration::kMaxHorseStaminasPerSecond;
      bool sent = false;
      {
        std::lock_guard<std::mutex> lock(network_mutex);
        sent = client->peer().send_horse_stamina(
            id, horse.epoch, std::min(stamina, integration::kHorseMaxStaminaReport));
      }
      if (sent) {
        horse.sent_stamina = stamina;
      }
    }
  }
  if (!authority) {
    // A replica shows the server's health (its own game's blows count only on the simulator).
    if (server_state == 0 && server_health > 0 && std::fabs(server_health - native_health) > 1) {
      health::show_health(soul, server_health);
    }
    return false;
  }
  // A resource's health (horse.health = ...): the server's value changed and is not this game's
  // own last report, so the body takes it (0.2.2.1: a blow after `health = 3` reported 100
  // minus the blow, undoing the resource's change).
  const bool server_changed = std::fabs(server_health - horse.seen_server_health) >= 0.01f;
  horse.seen_server_health = server_health;
  if (server_changed && server_state == 0 && native_state == 0 && server_health > 0 &&
      std::fabs(server_health - horse.sent_health) >= kHorseHealthReportStep &&
      std::fabs(server_health - native_health) > 1) {
    health::show_health(soul, server_health);
    horse.sent_health = server_health;
    log("HORSE health horse=%u takes the server's %.1f", id, server_health);
    // This tick's reading is from before; the next one reports from the new health.
    return false;
  }
  if (knock_out && now >= horse.next_immortality_check) {
    horse.next_immortality_check = now + kHorseImmortalitySeconds;
    knockout::keep_creature_immortal(soul);
  }
  const bool changed = native_state != horse.sent_state ||
                       std::fabs(native_health - horse.sent_health) >= kHorseHealthReportStep;
  if (!changed || now < horse.next_health_report) {
    return false;
  }
  horse.next_health_report = now + kHorseHealthReportSeconds;
  const bool state_changed = native_state != horse.sent_state;
  bool sent = false;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    sent = client->peer().send_horse_health(id, horse.epoch, native_health,
                                            std::uint8_t(native_state));
  }
  if (sent) {
    horse.sent_health = native_health;
    horse.sent_state = native_state;
  }
  if (state_changed) {
    log("HORSE health horse=%u reports %.1f state=%d sent=%u", id, native_health, native_state,
        unsigned(sent));
  }
  return false;
}

// The horse's order (horses/orders.cjs): its simulator walks it with the game's own
// movement on the navmesh (npcs/npc_walk.hpp), as an NPC's: a moveTo once, a patrol leg by leg
// ("<seq>|patrol|<speed>|<mode>|<radius>|<waitSeconds>|<pathfinding>|<x,y,z;...>", the mode
// once, loop or pingPong), and a hold (no text) stops it.
std::vector<std::string> split_text(const std::string& text, char separator) {
  std::vector<std::string> parts;
  std::size_t at = 0;
  while (true) {
    const std::size_t end = text.find(separator, at);
    parts.push_back(text.substr(at, end == std::string::npos ? std::string::npos : end - at));
    if (end == std::string::npos) {
      return parts;
    }
    at = end + 1;
  }
}

int horse_speed_of(const std::string& speed) {
  return speed == "run" ? 3 : speed == "jog" ? 2 : 1;
}

void walk_horse_leg(core::EntityId id, NativeHorse& horse, void* entity) {
  const float* tm = slot<const float*(__fastcall*)(void*)>(entity, 32)(entity);
  const float from[3] = {tm[3], tm[7], tm[11]};
  const core::Vec3& point = horse.patrol[horse.leg];
  const float to[3] = {point.x, point.y, point.z};
  const bool walks = kcds::runtime::npcs::walk::walk_to(entity, id, horse.order_sequence, from, to,
                                                        horse.order_speed);
  horse.leg_starts_at = 0;
  log("HORSE order horse=%u patrol leg %zu/%zu %s", id, horse.leg + 1, horse.patrol.size(),
      walks ? "walks" : "refused by the game");
}

void apply_horse_intent(core::EntityId id, NativeHorse& horse, void* entity,
                        const std::string& intent) {
  namespace walk = kcds::runtime::npcs::walk;
  horse.patrol.clear();
  horse.leg_starts_at = 0;
  const auto parts = split_text(intent, '|');
  if (intent.empty() || parts.size() < 2) {
    walk::stop(entity);
    log("HORSE order horse=%u holds", id);
    return;
  }
  horse.order_sequence = std::uint32_t(std::strtoul(parts[0].c_str(), nullptr, 10));
  if (parts[1] == "moveTo") {
    walk::Order order;
    if (!walk::parse_order(intent, order)) {
      return;
    }
    const float* tm = slot<const float*(__fastcall*)(void*)>(entity, 32)(entity);
    const float from[3] = {tm[3], tm[7], tm[11]};
    const bool walks =
        walk::walk_to(entity, id, order.sequence, from, order.destination, order.speed);
    log("HORSE order horse=%u moveTo (%.1f %.1f %.1f) %s", id, order.destination[0],
        order.destination[1], order.destination[2], walks ? "walks" : "refused by the game");
    return;
  }
  if (parts[1] != "patrol" || parts.size() < 8) {
    log("HORSE order horse=%u '%s' not carried out", id, parts[1].c_str());
    return;
  }
  horse.order_speed = horse_speed_of(parts[2]);
  horse.patrol_mode = parts[3];
  horse.patrol_wait = std::strtod(parts[5].c_str(), nullptr);
  for (const auto& point : split_text(parts[7], ';')) {
    const auto xyz = split_text(point, ',');
    if (xyz.size() == 3) {
      horse.patrol.push_back({std::strtof(xyz[0].c_str(), nullptr),
                              std::strtof(xyz[1].c_str(), nullptr),
                              std::strtof(xyz[2].c_str(), nullptr)});
    }
  }
  horse.leg = 0;
  horse.leg_step = 1;
  if (!horse.patrol.empty()) {
    walk_horse_leg(id, horse, entity);
  }
}

// A walk of a horse simulated here ended (npcs/native_npcs report_finished_walks): a patrol
// waits at the point, then goes on to the next leg (tick_horse_order).
void horse_walk_finished(core::EntityId id, std::uint32_t sequence, std::uint8_t status,
                         double now) {
  const auto found = native_horses.find(id);
  if (found == native_horses.end() || !found->second.authority ||
      sequence != found->second.order_sequence) {
    return;
  }
  NativeHorse& horse = found->second;
  namespace walk = kcds::runtime::npcs::walk;
  log("HORSE order horse=%u walk ended status=%u", id, unsigned(status));
  if (status == walk::kWalkReplaced) {
    return;
  }
  // The server hears that the order ended (horseIntentDone): a moveTo's walk, or the last leg of
  // a patrol that runs once.
  const auto report = [&](std::uint8_t game_status) {
    const auto ended = game_status == walk::kWalkReached  ? integration::kNpcIntentReached
                       : game_status == walk::kWalkNoPath ? integration::kNpcIntentFailed
                                                          : integration::kNpcIntentBlocked;
    std::lock_guard<std::mutex> lock(network_mutex);
    client->peer().send_npc_intent_done(id, horse.epoch, sequence, ended);
  };
  if (horse.patrol.empty()) {
    report(status);
    return;
  }
  const std::size_t count = horse.patrol.size();
  const bool last = horse.leg_step > 0 ? horse.leg + 1 == count : horse.leg == 0;
  if (last) {
    if (horse.patrol_mode == "once") {
      horse.patrol.clear();
      report(status);
      return;
    }
    if (horse.patrol_mode == "pingPong") {
      horse.leg_step = -horse.leg_step;
    }
  }
  if (horse.patrol_mode == "loop" && last) {
    horse.leg = 0;
  } else {
    horse.leg = std::size_t(std::int64_t(horse.leg) + horse.leg_step);
  }
  horse.leg_starts_at = now + horse.patrol_wait;
}

void tick_horse_order(core::EntityId id, NativeHorse& horse, void* entity,
                      const std::string& intent, double now) {
  if (intent != horse.intent) {
    horse.intent = intent;
    apply_horse_intent(id, horse, entity, intent);
    return;
  }
  if (!horse.patrol.empty() && horse.leg_starts_at > 0 && now >= horse.leg_starts_at) {
    walk_horse_leg(id, horse, entity);
  }
}

template <class Entities>
void retire_native_horses(const Entities& entities, void* entity_system) {
  for (auto it = native_horses.begin(); it != native_horses.end();) {
    if (entities.count(it->first)) {
      ++it;
      continue;
    }
    if (get_entity(entity_system, it->second.engine_id)) {
      detach_replica_rider(it->first, it->second);
      release_owned_horse(it->first, it->second);
      record_character_retirement(entity_system, it->second.engine_id);
      slot<void(__fastcall*)(void*, std::uint32_t, bool)>(entity_system, 19)(
          entity_system, it->second.engine_id, false);
    } else {
      owned_horse.forget(it->first);
    }
    {
      std::lock_guard<std::shared_mutex> lock(motion_mutex);
      horse_captures.erase(it->second.skeleton);
      remote_inputs.erase(it->second.skeleton);
      animation_stop_fences.erase(it->second.skeleton);
    }
    kcds::runtime::riding::horse_capacity::forget(it->second.carry_soul);
    kcds::runtime::riding::horse_speed_scale::forget(it->second.speed_body);
    kcds::runtime::buffs::horses::forget(it->first);
    log("HORSE removed network=%u", it->first);
    horse_retry_at.erase(it->first);
    it = native_horses.erase(it);
  }
  for (auto it = horse_retry_at.begin(); it != horse_retry_at.end();)
    if (!entities.count(it->first))
      it = horse_retry_at.erase(it);
    else
      ++it;
}

// Whether a HorseRiders list (network ids, comma-separated) holds the player.
bool rider_listed(const std::string& list, core::EntityId player) {
  if (player == core::kInvalidEntity || list.empty()) {
    return false;
  }
  std::size_t start = 0;
  while (start <= list.size()) {
    const std::size_t end = std::min(list.find(',', start), list.size());
    const std::string item = list.substr(start, end - start);
    if (!item.empty() && item == std::to_string(player)) {
      return true;
    }
    start = end + 1;
  }
  return false;
}

// The game's generic horse soul (Horse.lua's), what a horse without a soul of its own is.
constexpr char kGenericHorseSoul[] = "fc0e6251-b314-4398-b83f-3a66a52961ee";

// A soul GUID of the game's tables in its text form (8-4-4-4-12 hex digits): a named horse of the
// game (Horse.spawn's soul), spawned with that soul so it has the soul's own coat, mane and tail.
bool soul_guid(const std::string& soul) {
  if (soul.size() != 36) {
    return false;
  }
  for (std::size_t i = 0; i < soul.size(); ++i) {
    const char c = soul[i];
    const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    if (dash ? c != '-' : !std::isxdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

template <class Entities>
void tick_native_horses(const Entities& entities, void* actor_system, void* entity_system,
                        double now) {
  void* const local_actor = local_combat_actor.load();
  const bool player_horses = player_horses_installed && local_actor;
  {
    // The horse the local player rides, when this client simulates it (horse trample).
    auto ridden = local_rider_horse ? native_horses.find(local_rider_horse) : native_horses.end();
    local_ridden_horse_engine.store(
        ridden != native_horses.end() && ridden->second.authority ? ridden->second.engine_id : 0u);
  }
  for (const auto& [id, e] : entities) {
    if (e.type != core::type_id("NetworkHorse") || e.fields.size() < 11)
      continue;
    const auto p = std::get<core::Vec3>(e.fields[0]);
    const auto q = std::get<core::Quat>(e.fields[1]);
    const auto& soul = std::get<std::string>(e.fields[2]);
    // A soul of the game's tables is spawned as that soul (its sharedSoulGuid, as Horse.lua
    // reads it); anything else that is not the generic horse is refused, never spawned as
    // another horse.
    const bool own_soul = soul_guid(soul) && soul != kGenericHorseSoul;
    if (!soul.empty() && soul != "horse" && soul != kGenericHorseSoul && !own_soul) {
      if (!horse_retry_at.count(id)) {
        log("HORSE unsupported custom soul=%s network=%u", soul.c_str(), id);
        horse_retry_at[id] = now + 3600;
      }
      continue;
    }
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
      continue;
    auto it = native_horses.find(id);
    if (it != native_horses.end() && !get_entity(entity_system, it->second.engine_id)) {
      // The game removed the body itself: nothing of it is left to let go.
      owned_horse.forget(id);
      {
        std::lock_guard<std::shared_mutex> lock(motion_mutex);
        horse_captures.erase(it->second.skeleton);
        remote_inputs.erase(it->second.skeleton);
        animation_stop_fences.erase(it->second.skeleton);
      }
      kcds::runtime::riding::horse_capacity::forget(it->second.carry_soul);
      kcds::runtime::riding::horse_speed_scale::forget(it->second.speed_body);
      kcds::runtime::buffs::horses::forget(id);
      native_horses.erase(it);
      it = native_horses.end();
    }
    if (it == native_horses.end()) {
      if (!character_spawns_ready()) {
        continue;
      }
      if (horse_retry_at.count(id) && now < horse_retry_at[id])
        continue;
      horse_retry_at[id] = now + 5;
      char name[64];
      sprintf_s(name, "kcd2s_horse_%u", id);
      core::Vec3 scale{1, 1, 1};
      using CreateActor =
          void*(__fastcall*)(void*, unsigned short, const char*, const char*, const core::Vec3*,
                             const core::Quat*, const core::Vec3*, std::uint32_t);
      engine_stage = "horse native CreateActor";
      // The real animal lifecycle stays intact; the C_Player-only role hook is untouched. Our
      // synced horse passes the spawn hook that keeps the game's own animals out.
      void* actor = nullptr;
      if (own_soul) {
        // The Horse class's entity with the soul set before its init, then its actor
        // (IActorSystem slot 3: GetActor(entity id)).
        const float position[3] = {p.x, p.y, p.z};
        const float rotation[4] = {q.x, q.y, q.z, q.w};
        void* body = kcds::runtime::npcs::spawn_body(name, "Horse", soul, position, rotation);
        const std::uint32_t body_id =
            body ? *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(body) + 12) : 0;
        actor = body_id ? slot<void*(__fastcall*)(void*, std::uint32_t)>(actor_system, 3)(
                              actor_system, body_id)
                        : nullptr;
        log("HORSE soul %s network=%u spawned %s", soul.c_str(), id,
            actor ? "" : "without an actor");
      } else {
        kcds::runtime::npcs::OwnSpawn own;
        actor =
            slot<CreateActor>(actor_system, 5)(actor_system, 0, name, "Horse", &p, &q, &scale, 0);
      }
      if (!actor) {
        log("HORSE native factory refused network=%u", id);
        continue;
      }
      if ((*reinterpret_cast<void***>(actor))[7] != native_horse_init) {
        log("HORSE factory returned unexpected class; adapter stopped");
        failed = true;
        return;
      }
      auto entity = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + 56);
      if (!entity) {
        log("HORSE native actor has no entity; adapter stopped");
        failed = true;
        return;
      }
      const auto engine_id =
          *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(entity) + 12);
      auto character = slot<void*(__fastcall*)(void*, unsigned)>(entity, 101)(entity, 0);
      record_character_created(engine_id, entity);
      it = native_horses.emplace(id, NativeHorse{engine_id, p, q}).first;
      it->second.actor = actor;
      it->second.skeleton =
          character ? slot<void*(__fastcall*)(void*)>(character, 5)(character) : nullptr;
      it->second.animated = slot<void*(__fastcall*)(void*)>(actor, 90)(actor);
      horse_retry_at.erase(id);
      log("HORSE native C_Horse network=%u engine=0x%x actor=%p character=%p name=%s", id,
          engine_id, actor, character, std::get<std::string>(e.fields[3]).c_str());
    }
    auto& horse = it->second;
    // The most its saddlebags carry: the soul of the body as it is now.
    if (e.fields.size() > integration::HorseMaxCarryWeight && horse.actor) {
      void* soul = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(horse.actor) + 1640);
      if (soul != horse.carry_soul) {
        kcds::runtime::riding::horse_capacity::forget(horse.carry_soul);
        horse.carry_soul = soul;
      }
      kcds::runtime::riding::horse_capacity::set(
          soul, std::get<float>(e.fields[integration::HorseMaxCarryWeight]));
      // Its stamina and speed.
      if (e.fields.size() > integration::HorseSpeed) {
        namespace capacity = kcds::runtime::riding::horse_capacity;
        capacity::set_stat(soul, capacity::kMaxStamina,
                           std::get<float>(e.fields[integration::HorseMaxStamina]));
        capacity::set_speed(soul, std::get<float>(e.fields[integration::HorseSpeed]));
      }
    }
    // The look the server chose, so the horse is the same on every client; a new
    // look builds the body anew.
    if (e.fields.size() > integration::HorseLook) {
      const auto& look = std::get<std::string>(e.fields[integration::HorseLook]);
      if (!look.empty() && look != horse.look) {
        const bool built = kcds::runtime::npcs::apply_appearance_field(horse.actor, look);
        log("HORSE look horse=%u %s %s", id, look.c_str(), built ? "built" : "refused");
        horse.look = look;
        // The body built anew wears nothing yet: its tack goes on again.
        kcds::runtime::appearance::outfit::forget(id);
      }
    }
    // The tack the server chose: every body of the horse wears it, the rider's own
    // and the simulator's included, so the bridle and reins show for everyone.
    if (e.fields.size() > integration::HorseGear && horse.actor) {
      const auto& gear = std::get<std::string>(e.fields[integration::HorseGear]);
      if (!gear.empty()) {
        if (gear != horse.gear) {
          log("HORSE gear horse=%u %s", id,
              kcds::runtime::appearance::outfit::describe(gear).c_str());
          horse.gear = gear;
          horse.gear_names_logged = false;
        }
        if (void* body = get_entity(entity_system, horse.engine_id)) {
          // Its own dressing, not the player's tack change (riding/horse_tack.hpp).
          kcds::runtime::riding::horse_tack::WhileSyncing dressing;
          kcds::runtime::appearance::outfit::apply(id, horse.actor, body, gear);
        }
        // Once the new tack is on: which worn pieces show a name of their own (a piece put on
        // from a named row), for the log.
        if (!horse.gear_names_logged) {
          log_named_horse_pieces(id, horse);
        }
      }
    }
    // The players besides its owner allowed to ride it (Horse.allowRider): for the local player
    // among them the horse script's mountIsLegal is set (Horse.lua IsMountLegal), so its game
    // offers the plain mount; the saddlebags stay the owner's.
    // How dirty the server made it (Horse.dirt): put on its body and tack when it changes, and
    // again for a body made anew.
    if (e.fields.size() > integration::HorseDirt && horse.actor) {
      const float dirt = std::get<float>(e.fields[integration::HorseDirt]);
      if (dirt >= 0 && dirt != horse.dirt &&
          kcds::runtime::appearance::grime::set_dirt(horse.actor, dirt)) {
        horse.dirt = dirt;
        log("HORSE dirt network=%u dirt=%.2f", id, dirt);
      }
    }
    // The share of its pace the server gave it (Horse.speedScale), in every gait: on its body,
    // again for a body made anew.
    if (e.fields.size() > integration::HorseSpeedScale) {
      const float scale = std::get<float>(e.fields[integration::HorseSpeedScale]);
      void* body = get_entity(entity_system, horse.engine_id);
      if (body && (scale != horse.speed_scale || body != horse.speed_body)) {
        if (horse.speed_body && horse.speed_body != body) {
          kcds::runtime::riding::horse_speed_scale::forget(horse.speed_body);
        }
        kcds::runtime::riding::horse_speed_scale::set(body, scale);
        horse.speed_body = body;
        horse.speed_scale = scale;
        log("HORSE speed scale network=%u scale=%.2f", id, scale);
      }
    }
    // Who leads it on a rope (Horse.follow with rope); riding/lead_rope.cpp draws it.
    if (e.fields.size() > integration::HorseLead) {
      horse.lead = core::EntityId(std::get<std::int64_t>(e.fields[integration::HorseLead]));
    }
    if (e.fields.size() > integration::HorseLeadAt) {
      horse.lead_at = std::get<std::string>(e.fields[integration::HorseLeadAt]);
    }
    if (e.fields.size() > integration::HorseRiders) {
      const bool legal = rider_listed(std::get<std::string>(e.fields[integration::HorseRiders]),
                                      local_network_id.load());
      if (legal != horse.mount_legal) {
        if (void* body = get_entity(entity_system, horse.engine_id)) {
          const bool set = kcds::runtime::npcs::set_script_flag(body, "mountIsLegal", legal);
          horse.mount_legal = legal;
          log("HORSE riders horse=%u the local player may ride: %u (set %u)", id, unsigned(legal),
              unsigned(set));
        }
      }
    }
    // The server's buffs on the horse: every body of it carries them.
    if (e.fields.size() > integration::HorseBuffs && horse.actor) {
      void* soul = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(horse.actor) + 1640);
      kcds::runtime::buffs::horses::apply(
          id, soul, std::get<std::string>(e.fields[integration::HorseBuffs]), now);
    }
    // The skeleton, read anew every frame: building the body anew can give it a new character,
    // and the old skeleton is then gone. Its capture and replay move with it.
    void* skeleton = nullptr;
    if (void* body = get_entity(entity_system, horse.engine_id)) {
      if (auto c = slot<void*(__fastcall*)(void*, unsigned)>(body, 101)(body, 0)) {
        skeleton = slot<void*(__fastcall*)(void*)>(c, 5)(c);
      }
    }
    if (skeleton != horse.skeleton) {
      if (horse.skeleton) {
        std::lock_guard<std::shared_mutex> lock(motion_mutex);
        horse_captures.erase(horse.skeleton);
        remote_inputs.erase(horse.skeleton);
        animation_stop_fences.erase(horse.skeleton);
        log("HORSE skeleton horse=%u %p -> %p", id, horse.skeleton, skeleton);
      }
      horse.skeleton = skeleton;
      std::memset(horse.animation_sequences, 0, sizeof(horse.animation_sequences));
    }
    if (!horse.animated)
      horse.animated = slot<void*(__fastcall*)(void*)>(horse.actor, 90)(horse.actor);
    const auto own = local_network_id.load();
    // The server gave this horse to the local player (Horse.giveTo): offered as its own horse
    // once its body is ready, as the mount waits for the skeleton.
    const auto owner = std::get<std::int64_t>(e.fields[integration::HorseOwner]);
    if (player_horses && own != 0 && owner == std::int64_t(own) && horse.skeleton &&
        kcds::runtime::riding::horse_ready(horse.actor)) {
      owned_horse.offer(id);
    }
    // Given to someone else: a game that took the horse as its player's own after a theft (the
    // game does that for a player without a horse) lets it go, so giveTo decides in every game.
    if (owner != horse.owner) {
      const bool given_away = horse.owner >= 0 && owner != std::int64_t(own);
      horse.owner = owner;
      if (player_horses && given_away &&
          kcds::runtime::riding::release_player_horse(local_actor, horse.actor)) {
        owned_horse.forget(id);
        log("HORSE player horse let go network=%u: given to player %lld", id, owner);
      }
    }
    const auto simulator =
        core::EntityId(std::get<std::int64_t>(e.fields[integration::HorseSimulator]));
    const auto epoch = std::uint32_t(std::get<std::int64_t>(e.fields[integration::HorseEpoch]));
    const bool authority = simulator == own && own != 0;
    horse.authority = authority;
    if (epoch != horse.epoch) {
      horse.epoch = epoch;
      std::memset(horse.animation_sequences, 0, sizeof(horse.animation_sequences));
      log("HORSE authority horse=%u simulator=%u epoch=%u local=%u", id, simulator, epoch,
          unsigned(authority));
    }
    {
      std::lock_guard<std::shared_mutex> lock(motion_mutex);
      auto& captured = horse_captures[horse.skeleton];
      captured.animated = horse.animated;
      captured.authority = authority;
      if (authority)
        remote_inputs.erase(horse.skeleton);
    }
    if (tick_horse_health(id, horse, e.fields, authority, now)) {
      // Made anew on the next tick, standing, where the server has it.
      detach_replica_rider(id, horse);
      release_owned_horse(id, horse);
      owned_horse.forget(id);
      if (get_entity(entity_system, horse.engine_id)) {
        record_character_retirement(entity_system, horse.engine_id);
        slot<void(__fastcall*)(void*, std::uint32_t, bool)>(entity_system, 19)(
            entity_system, horse.engine_id, false);
      }
      {
        std::lock_guard<std::shared_mutex> lock(motion_mutex);
        horse_captures.erase(horse.skeleton);
        remote_inputs.erase(horse.skeleton);
        animation_stop_fences.erase(horse.skeleton);
      }
      kcds::runtime::buffs::horses::forget(id);
      native_horses.erase(id);
      horse_retry_at.erase(id);
      continue;
    }
    if (authority && e.fields.size() > integration::HorseIntent) {
      if (void* body = get_entity(entity_system, horse.engine_id)) {
        tick_horse_order(id, horse, body, std::get<std::string>(e.fields[integration::HorseIntent]),
                         now);
      }
    } else {
      // Another game simulates it: this one takes the order anew when it simulates again.
      horse.intent.clear();
      horse.patrol.clear();
    }
    if (authority) {
      if (horse.skeleton)
        capture_existing_animations(horse.skeleton, true);
      integration::HorseState state;
      state.source = own;
      state.horse = id;
      state.sequence = ++horse.sequence;
      auto entity = get_entity(entity_system, horse.engine_id);
      auto tm = slot<const float*(__fastcall*)(void*)>(entity, 32)(entity);
      state.position = {tm[3], tm[7], tm[11]};
      state.rotation = rotation(tm);
      {
        std::lock_guard<std::shared_mutex> lock(motion_mutex);
        const auto& c = horse_captures.at(horse.skeleton);
        state.velocity = c.motion.velocity;
        state.motion_rotation = c.motion.rotation;
        state.motion_type = c.motion.type;
        state.motion_flags = (c.motion.allow_strafe ? 1 : 0) | (c.motion.jumping ? 2 : 0);
        state.motion_valid = c.valid;
        state.mask = c.mask;
        std::memcpy(state.params.data(), c.params, sizeof(c.params));
        state.animations = local::encode_animation(c.animations);
      }
      {
        std::lock_guard<std::mutex> lock(network_mutex);
        client->peer().send_horse_state(std::move(state));
      }
    } else {
      if (horse_rider_transition(horse)) {
        horse.replica_transition = true;
        {
          std::lock_guard<std::shared_mutex> lock(motion_mutex);
          remote_inputs.erase(horse.skeleton);
        }
        continue;
      }
      if (horse.replica_transition) {
        horse.replica_transition = false;
        std::memset(horse.animation_sequences, 0, sizeof(horse.animation_sequences));
        log("HORSE transition released; reseeding current owner FIFO horse=%u tick=%llu", id,
            GetTickCount64());
      }
      const auto& raw = std::get<std::string>(e.fields[integration::HorseNativeState]);
      integration::HorseState state;
      const bool state_valid =
          !raw.empty() &&
          integration::decode_horse(std::vector<std::uint8_t>(raw.begin(), raw.end()), state) &&
          state.epoch == epoch && state.source == simulator && state.revision == requested_revision;
      engine_stage = "horse authoritative transform";
      // The replica seated on it, put on its seat every frame (seat_rider_on_horse); not while it
      // mounts or dismounts, which the game's own RiderSync plays.
      void* rider_entity = nullptr;
      if (horse.replica_rider) {
        const auto rider = remotes.find(horse.replica_rider);
        if (rider != remotes.end() && rider->second.engine_id &&
            native_human_mounted(rider->second.actor) &&
            !native_rider_transition(rider->second.actor)) {
          rider_entity = get_entity(entity_system, rider->second.engine_id);
        }
      }
      follow_replicated_horse(horse, get_entity(entity_system, horse.engine_id), rider_entity, p, q,
                              state_valid && state.motion_valid ? &state.velocity : nullptr, now);
      if (state_valid) {
        auto clips = local::decode_animation(state.animations);
        RemoteInputs remote;
        remote.animated = horse.animated;
        remote.valid = state.motion_valid;
        remote.motion.type = state.motion_type;
        remote.motion.velocity = state.velocity;
        remote.motion.rotation = state.motion_rotation;
        remote.motion.allow_strafe = (state.motion_flags & 1) != 0;
        remote.motion.jumping = (state.motion_flags & 2) != 0;
        remote.mask = state.mask;
        std::memcpy(remote.params, state.params.data(), sizeof(remote.params));
        remote.animations = *clips;
        {
          std::lock_guard<std::shared_mutex> lock(motion_mutex);
          remote_inputs.assign(horse.skeleton, remote);
        }
        if (remote.valid && horse.animated)
          original_movement_request(horse.animated, &remote.motion);
        if (horse.skeleton) {
          for (unsigned layer = 0; layer < 16; ++layer) {
            const auto& clip = (*clips)[layer];
            auto& applied = horse.animation_sequences[layer];
            if (!clip.sequence || clip.sequence <= applied)
              continue;
            replaying_animation = true;
            if (!applied || clip.clip < 0)
              original_clear_animation(horse.skeleton, layer);
            auto accepted = clip.clip < 0
                                ? 1
                                : original_start_animation(horse.skeleton, clip.clip, &clip.params);
            replaying_animation = false;
            if (accepted) {
              applied = clip.sequence;
              log("HORSE ANIMATION horse=%u layer=%u clip=%d sequence=%llu time=%.4f flags=0x%x "
                  "tick=%llu",
                  id, layer, clip.clip, clip.sequence, clip.params.normalized_time,
                  clip.params.flags, GetTickCount64());
            }
          }
          // A clip that is already on (its start applied) keeps its simulator's time.
          const auto& base = (*clips)[0];
          if (base.sequence && base.sequence == horse.animation_sequences[0]) {
            keep_clip_on_simulator_time(id, horse.skeleton, base);
          }
          for (unsigned i = 0; i < 17; ++i)
            if (state.mask & (1u << i))
              original_motion_param(horse.skeleton, int(i), state.params[i]);
        }
      }
    }
    // Test hook: the horse's head in its own space, every 250 ms, in the simulating game
    // and in the others, to compare the head of the copy with the owner's.
    static const bool head_trace =
        GetEnvironmentVariableW(L"KCDS_TEST_REPLICA_TRACE", nullptr, 0) > 0;
    if (head_trace && now >= horse.next_head_diag) {
      horse.next_head_diag = now + 0.25;
      namespace entity = kcds::runtime::props::entity;
      void* body = get_entity(entity_system, horse.engine_id);
      static int head_joint = -1;
      if (head_joint < 0 && body) {
        for (const char* name : {"Head", "Neck5"}) {
          if (head_joint < 0) {
            head_joint = entity::joint_id(body, name);
            if (head_joint >= 0) {
              log("HORSE HEAD joint=%d name=%s", head_joint, name);
            }
          }
        }
      }
      float head[3]{};
      float turn[4]{};
      if (body && head_joint >= 0 && entity::joint_pose_by_id(body, head_joint, head, turn)) {
        const float* tm = slot<const float*(__fastcall*)(void*)>(body, 32)(body);
        const float dx = head[0] - tm[3];
        const float dy = head[1] - tm[7];
        const float dz = head[2] - tm[11];
        // The offset in the horse's own axes (the matrix's columns: right, forward, up).
        const float right = dx * tm[0] + dy * tm[4] + dz * tm[8];
        const float forward = dx * tm[1] + dy * tm[5] + dz * tm[9];
        const float up = dx * tm[2] + dy * tm[6] + dz * tm[10];
        // The base layer's clips (SkeletonAnim slot 15 count, slot 18 entry: clip +16) and how
        // far each is (the progress getter, as the capture reads it).
        std::string clips;
        if (horse.skeleton) {
          const auto count =
              slot<unsigned(__fastcall*)(void*, unsigned)>(horse.skeleton, 15)(horse.skeleton, 0);
          for (unsigned i = 0; i < count && i < 4; ++i) {
            auto raw = slot<const std::uint8_t*(__fastcall*)(void*, unsigned, unsigned)>(
                horse.skeleton, 18)(horse.skeleton, 0, i);
            const int clip = *reinterpret_cast<const int*>(raw + 16);
            const float progress = native_animation_progress(horse.skeleton, raw);
            char entry[48];
            std::snprintf(entry, sizeof(entry), " %d@%.3f", clip, progress);
            clips += entry;
          }
        }
        log("HORSE HEAD horse=%u authority=%u right=%.2f forward=%.2f up=%.2f clips=%s tick=%llu",
            id, unsigned(authority), right, forward, up, clips.c_str(), GetTickCount64());
      }
    }
    if (now >= horse.next_diag) {
      horse.next_diag = now + 2;
      auto tm =
          slot<const float*(__fastcall*)(void*)>(get_entity(entity_system, horse.engine_id),
                                                 32)(get_entity(entity_system, horse.engine_id));
      log("HORSE motion horse=%u authority=%u native=(%.2f %.2f %.2f) server=(%.2f %.2f %.2f) "
          "rider=%lld skeleton=%p animated=%p",
          id, unsigned(authority), tm[3], tm[7], tm[11], p.x, p.y, p.z,
          std::get<std::int64_t>(e.fields[integration::HorseRider]), horse.skeleton,
          horse.animated);
    }
    horse.position = p;
    horse.orientation = q;
  }
  if (player_horses) {
    apply_owned_horse(local_actor);
  }
}

// Detach existing native pairs while both bodies still exist. Removal runs before the
// normal rider tick; waiting until that tick would already have forgotten the rider's actor.
template <class Entities>
void detach_retiring_riders(const Entities& entities) {
  for (auto& [id, horse] : native_horses) {
    if (!horse.replica_rider) {
      continue;
    }
    const auto latest = entities.find(id);
    const bool horse_visible = latest != entities.end();
    const auto rider =
        horse_visible
            ? core::EntityId(std::get<std::int64_t>(latest->second.fields[integration::HorseRider]))
            : core::EntityId{0};
    if (horse_visible && rider == horse.replica_rider && entities.count(rider)) {
      continue;
    }
    detach_replica_rider(id, horse);
  }
}

// MCP verifies native Human v204/v205; C_Player wrappers
// resolve uniquely before installation. Engine thread only.
template <class Entities>
void tick_native_riders(const Entities& entities, core::EntityId own, void* local_actor,
                        void* actor_system, double now) {
  (void)actor_system;
  detach_retiring_riders(entities);
  if (requested_local_horse) {
    const auto mounted = native_human_mounted(local_actor) != 0;
    if (mounted)
      observed_local_mount = true;
    if (requested_local_mount && ((observed_local_mount && !mounted) ||
                                  (!observed_local_mount && now - local_mount_started > 5))) {
      requested_local_mount = false;
      next_mount_report = 0;
    }
    auto e = entities.find(requested_local_horse);
    if (e == entities.end()) {
      requested_local_horse = 0;
      requested_local_mount = false;
    } else {
      const auto rider =
          core::EntityId(std::get<std::int64_t>(e->second.fields[integration::HorseRider]));
      if ((requested_local_mount ? rider != own : rider == own) && now >= next_mount_report) {
        next_mount_report = now + .5;
        std::lock_guard<std::mutex> lock(network_mutex);
        client->peer().send_horse_mount(own, requested_local_horse, requested_local_mount);
      }
      if (!requested_local_mount && rider != own)
        requested_local_horse = 0;
    }
  }
  for (auto& [id, h] : native_horses) {
    auto e = entities.find(id);
    if (e == entities.end())
      continue;
    const auto rider =
        core::EntityId(std::get<std::int64_t>(e->second.fields[integration::HorseRider]));
    // The horse's character loads after the horse: a replica created for a player who already
    // rides (a client joining later) has none in its first frames, and the game's RiderSync of
    // the mount reads the horse's skeleton: mounted then, the engine faulted and
    // nothing synchronized on that client any more. The mount waits for the skeleton.
    if (rider && rider != own && now >= h.next_mount_attempt && h.skeleton) {
      auto r = remotes.find(rider);
      if (r == remotes.end() || !r->second.skeleton)
        continue;
      if (h.replica_rider == rider && native_human_mounted(r->second.actor))
        continue;
      h.next_mount_attempt = now + 1;
      {
        std::lock_guard<std::shared_mutex> lock(motion_mutex);
        remote_inputs.erase(r->second.skeleton);
        remote_inputs.erase(h.skeleton);
      }
      const auto accepted = native_remote_horse_mount(r->second.actor, h.actor, 0);
      if (!accepted) {
        log("HORSE replica native mount refused horse=%u rider=%u mounted=%u", id, rider,
            unsigned(native_human_mounted(r->second.actor) != 0));
      }
      if (accepted) {
        h.replica_rider = rider;
        log("HORSE replica native mount horse=%u rider=%u accepted=%u tick=%llu", id, rider,
            unsigned(accepted), GetTickCount64());
      }
      retain_local_input(local_actor);
      retain_local_view();
    }
  }
}

template void detach_retiring_riders<MirrorEntities>(const MirrorEntities&);

template void retire_native_horses<MirrorEntities>(const MirrorEntities&, void*);

template void tick_native_horses<MirrorEntities>(const MirrorEntities&, void*, void*, double);
template void tick_native_riders<MirrorEntities>(const MirrorEntities&, core::EntityId, void*,
                                                 void*, double);

}  // namespace kcds::game_client
