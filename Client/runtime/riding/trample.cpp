// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "trample.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../replicas/motion_capture.hpp"
#include "../props/prop_entity.hpp"
#include "../replicas/remote.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace kcds::game_client {

namespace integration = kcd2s::integration;

// Horse trample: the engine id of the horse the local player rides while this
// client simulates it (0 otherwise), set by the horse tick; and the last relay per victim.
std::atomic<unsigned> local_ridden_horse_engine{0};
// The engine id of the horse replica a remote rider sits on (0 when none); engine thread.
unsigned ridden_horse_engine(core::EntityId rider);
// The actor of that horse replica (nullptr when none); engine thread.
void* ridden_horse_actor(core::EntityId rider);
std::unordered_map<core::EntityId, ULONGLONG> trample_relayed_at;
// The trampled player's knockdown on the rider's client is the game's own reaction on the replica;
// the owner's client meanwhile clears its clips (its fall is that same reaction, not a clip), and
// replaying those clears left the replica in a T-pose. Until then a cleared layer is not
// replayed for that player; the next real clip (getting up) plays as usual.
std::unordered_map<core::EntityId, ULONGLONG> trample_fall_until;

bool trample_falling(core::EntityId player) {
  auto fall = trample_fall_until.find(player);
  return fall != trample_fall_until.end() && GetTickCount64() < fall->second;
}
constexpr ULONGLONG kTrampleGapMs = 1000;

// The native trample damage: the actor collision handler first
// builds an RPG S_CollisionHitData (104 bytes) from the physics collision and passes it to the
// second body's soul RPG object (soul vfunc 57, then its vfunc 44 =). That creates an
// RPG event with a C_CollisionHitCause and collision effects for both souls; the handler submits
// it to the RPG world events. Only then does copy the resulting health and stamina
// deltas into the two type-3 HitInfos (+44/+48), which carry the reaction, not the damage.
// The rider's client runs all of this against the victim's replica, so the victim's own soul
// never took the damage. The trample relay therefore also carries the collision velocities and
// the victim's client submits the same native collision event with its own souls.
//
// S_CollisionHitData: two bodies of 32 bytes (+0 soul id, +8 soul vfunc 134(0), +16 world
// velocity, +28 body flag), then +64 contact point, +76 normal and +88 the normalized velocity
// of the first physics body.
constexpr std::size_t kCollisionHitDataSize = 104;
constexpr std::size_t kCollisionBodySize = 32;
using CollisionHitData = std::array<std::uint8_t, kCollisionHitDataSize>;
CreateCollisionHitEvent original_create_collision_hit_event = nullptr;
// The last S_CollisionHitData on this thread. The collision handler queues its HitInfos right
// after creating the event, so the relay finds the data of the same collision here.
thread_local CollisionHitData last_collision_hit{};
thread_local bool last_collision_hit_valid = false;

void** __fastcall hooked_create_collision_hit_event(void* self, void** event, const void* data) {
  std::memcpy(last_collision_hit.data(), data, kCollisionHitDataSize);
  last_collision_hit_valid = true;
  return original_create_collision_hit_event(self, event, data);
}

// The relayed velocities travel in the 12 bytes after the HitInfo (the melee outcome bytes,
// unused by a collision hit) as six int16 values in centimetres per second: horse, then victim.
constexpr std::size_t kTrampleVelocityOffset = 160;
constexpr float kTrampleVelocityScale = 100.0f;
// NativeReactionData[172] is 1 when the horse is the first body of the collision data.
constexpr std::size_t kTrampleHorseFirstOffset = 172;

void pack_trample_velocity(integration::NativeReactionData& data, std::size_t body_index,
                           const float* velocity) {
  for (std::size_t axis = 0; axis < 3; ++axis) {
    const float scaled = std::clamp(velocity[axis] * kTrampleVelocityScale, -32767.0f, 32767.0f);
    const auto packed = static_cast<std::int16_t>(std::lround(scaled));
    const std::size_t at = kTrampleVelocityOffset + (body_index * 3 + axis) * 2;
    std::memcpy(data.data() + at, &packed, 2);
  }
}

// The soul id the collision data stores for a soul (*soul vfunc 0).
std::uint64_t collision_soul_id(void* soul) {
  return *slot<const std::uint64_t*(__fastcall*)(void*)>(soul, 0)(soul);
}

// A soul's derived stat. The collision effect weighs both bodies'
// velocities with stat 38 of their souls: vcm = (m1 v1 + m2 v2) / (m1 + m2), and the energy is
// |vcm - v_own| times a config factor minus an offset, so the damage scales with this stat.
SoulStat native_soul_stat = nullptr;
constexpr int kCollisionMassStat = 38;

float collision_mass(void* soul) {
  if (!soul || !native_soul_stat) {
    return -1.0f;
  }
  alignas(16) std::uint8_t context[64] = {};
  return native_soul_stat(soul, kCollisionMassStat, context);
}

// Logs one collision data (diagnostics: the victim's rebuilt data against the rider's).
void log_collision(const char* side, const std::uint8_t* collision, float first_mass,
                   float second_mass) {
  for (std::size_t body = 0; body < 2; ++body) {
    const std::uint8_t* at = collision + body * kCollisionBodySize;
    std::uint64_t id = 0;
    unsigned value = 0;
    float velocity[3];
    std::memcpy(&id, at, 8);
    std::memcpy(&value, at + 8, 4);
    std::memcpy(velocity, at + 16, 12);
    log("COMBAT TRAMPLE %s body=%zu soul=%llx value=%u velocity=(%.2f %.2f %.2f) flag=%u mass=%.3f",
        side, body, static_cast<unsigned long long>(id), value, velocity[0], velocity[1],
        velocity[2], unsigned(at[28]), body == 0 ? first_mass : second_mass);
  }
  float point[3];
  float normal[3];
  float direction[3];
  std::memcpy(point, collision + 64, 12);
  std::memcpy(normal, collision + 76, 12);
  std::memcpy(direction, collision + 88, 12);
  log("COMBAT TRAMPLE %s point=(%.2f %.2f %.2f) normal=(%.2f %.2f %.2f) dir=(%.2f %.2f %.2f)", side,
      point[0], point[1], point[2], normal[0], normal[1], normal[2], direction[0], direction[1],
      direction[2]);
}

// Copies the horse and victim collision velocities into the relayed reaction data.
// Returns false when the last collision data on this thread is not about this victim.
bool pack_trample_collision(integration::NativeReactionData& data, void* victim_actor) {
  if (!last_collision_hit_valid || !victim_actor) {
    return false;
  }
  auto victim_soul = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(victim_actor) + 1640);
  if (!victim_soul) {
    return false;
  }
  const std::uint64_t victim_id = collision_soul_id(victim_soul);
  std::uint64_t first_id = 0;
  std::uint64_t second_id = 0;
  std::memcpy(&first_id, last_collision_hit.data(), 8);
  std::memcpy(&second_id, last_collision_hit.data() + kCollisionBodySize, 8);
  bool horse_first;
  if (second_id == victim_id) {
    horse_first = true;
  } else if (first_id == victim_id) {
    horse_first = false;
  } else {
    return false;
  }
  const std::size_t horse_body = horse_first ? 0 : kCollisionBodySize;
  const std::size_t victim_body = horse_first ? kCollisionBodySize : 0;
  float velocity[3];
  std::memcpy(velocity, last_collision_hit.data() + horse_body + 16, 12);
  pack_trample_velocity(data, 0, velocity);
  std::memcpy(velocity, last_collision_hit.data() + victim_body + 16, 12);
  pack_trample_velocity(data, 1, velocity);
  data[kTrampleHorseFirstOffset] = horse_first ? 1 : 0;
  void* horse_actor = ridden_horse_actor(local_network_id.load());
  void* horse_soul = horse_actor
                         ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(horse_actor) + 1640)
                         : nullptr;
  const float horse_mass = collision_mass(horse_soul);
  const float victim_mass = collision_mass(victim_soul);
  log_collision("rider", last_collision_hit.data(), horse_first ? horse_mass : victim_mass,
                horse_first ? victim_mass : horse_mass);
  return true;
}

// On the victim's client the damage is the game's own collision result from the rider's client:
// rebuilt here with the local souls, the collision effect took only 0-0.6 health of
// the local player while the rider's client computed about 25 for the same collision. The
// relayed HitInfo carries that native result (copies the event's health and stamina
// losses to +44/+48), and the victim's soul takes it with the soul's own damage call: the
// interface at soul+240, vtable slot 27, as the script's soul:DealDamage calls it
// with stamina, then health (DealDamage(10, 5) took 5 health), the attacker's soul, a flag
// and a default damage type (freed).
using SoulDealDamage = void(__fastcall*)(void* damage, float stamina, float health, void* attacker,
                                         char flag, void* type);
constexpr std::size_t kSoulDamage = 240;
constexpr std::size_t kSoulDealDamageSlot = 27;
constexpr std::size_t kHitHealthLoss = 44;
constexpr std::size_t kHitStaminaLoss = 48;
DamageTypeInit damage_type_init = nullptr;
DamageTypeFree damage_type_free = nullptr;

// Returns false when nothing was applied.
bool apply_trample_damage(const integration::NativeReactionData& data, void* horse_actor,
                          void* victim_actor) {
  if (!victim_actor || !damage_type_init || !damage_type_free) {
    return false;
  }
  auto victim_soul =
      *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(victim_actor) + 1640);
  void* horse_soul = horse_actor
                         ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(horse_actor) + 1640)
                         : nullptr;
  float health = 0;
  float stamina = 0;
  std::memcpy(&health, data.data() + kHitHealthLoss, 4);
  std::memcpy(&stamina, data.data() + kHitStaminaLoss, 4);
  if (!victim_soul || !(health > 0.0f || stamina > 0.0f)) {
    return false;
  }
  void* damage = victim_soul + kSoulDamage;
  alignas(16) std::uint8_t type[64] = {};
  damage_type_init(type);
  slot<SoulDealDamage>(damage, kSoulDealDamageSlot)(damage, stamina, health, horse_soul, 0, type);
  damage_type_free(type);
  return true;
}

// A collision HitInfo from the locally simulated horse onto a remote player is relayed to that
// player's owner, which queues it through GameRules there (handles type 3 itself).
// The rider's own copy of the HitInfo also reaches the replica through GameRules, so the replica
// gets the game's own collision reaction like its owner: the collision impulse, the
// fall clip 3920 and its "Ragdollize" animation event (a script RagDollize or the clip alone
// dropped it elsewhere or not at all). While it lies its owner's position, movement and clips are
// not forced on it (that dragged the ragdoll through the air, live); when its owner gets up it is
// returned with the "alive" physicalization profile (live probe: that stood a ragdoll up again)
// and replays its owner's getting up.
std::unordered_set<core::EntityId> trample_ragdoll_pending;
std::unordered_map<core::EntityId, ULONGLONG> trample_ragdoll_started;
constexpr ULONGLONG kTrampleRagdollMinMs = 800;
// The owner may lie for more than 4 s (it got up 4.4 s after the hit; a release at 4 s showed
// its empty layers, a T-pose, until then).
constexpr ULONGLONG kTrampleRagdollMaxMs = 10000;
// The actor's physicalization profile, as the script binding SetPhysicalizationProfile sets it
// the actor's physics controller at +40, its vtable slot 24 with the request 8.
constexpr std::size_t kActorPhysicsController = 40;
constexpr std::size_t kSetPhysicsProfile = 24;
constexpr int kSetPhysicsProfileRequest = 8;

bool set_physics_profile(void* actor, PhysicsProfile profile) {
  void* controller =
      actor ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + kActorPhysicsController)
            : nullptr;
  if (!controller) {
    return false;
  }
  using SetProfile = void(__fastcall*)(void*, int, std::uint8_t);
  slot<SetProfile>(controller, kSetPhysicsProfile)(controller, kSetPhysicsProfileRequest,
                                                   static_cast<std::uint8_t>(profile));
  return true;
}

// The pose of a ragdoll, synchronized from its owner: the owner's ragdoll and the
// replica's were simulated apart, so the replica lay in another pose than its owner (live video:
// face down for the owner, sitting on the rider's client). While the owner is a ragdoll (an
// articulated physical entity) its client sends the position and rotation of every physics part
// relative to the entity (pe_params_part, read with GetParams); the replica's parts are set to
// them (SetParams), with its owner's position and rotation.
// entity vfunc 632 is its physical entity; physical entity vtable [1] GetType (articulated 6),
// [4] SetParams (CPhysicalEntity::SetParams), [5] GetParams (type 6 copies
// part pos +16, q +28, scale +44); builds a pe_params_part (160 bytes, unused marks).
PartParamsInit native_part_params_init = nullptr;
constexpr std::size_t kEntityPhysics = 632 / sizeof(void*);
constexpr std::size_t kPhysicsGetType = 1;
constexpr std::size_t kPhysicsSetParams = 4;
constexpr std::size_t kPhysicsGetParams = 5;
constexpr int kPhysicsArticulated = 6;
constexpr std::size_t kPartParamsSize = 160;
constexpr std::size_t kPartParamsIndex = 8;
constexpr std::size_t kPartParamsTransform = 16;
constexpr unsigned kRagdollPartsMax = 48;
constexpr char kRagdollPoseTag[] = "KSR1";

void* articulated_physics(void* entity) {
  void* physics =
      entity ? slot<void*(__fastcall*)(void*)>(entity, kEntityPhysics)(entity) : nullptr;
  if (!physics ||
      slot<int(__fastcall*)(void*)>(physics, kPhysicsGetType)(physics) != kPhysicsArticulated) {
    return nullptr;
  }
  return physics;
}

std::string capture_ragdoll_pose(void* entity) {
  void* physics = native_part_params_init ? articulated_physics(entity) : nullptr;
  if (!physics) {
    return {};
  }
  std::string pose = kRagdollPoseTag;
  for (unsigned part = 0; part < kRagdollPartsMax; ++part) {
    alignas(16) std::uint8_t params[kPartParamsSize] = {};
    native_part_params_init(params);
    std::memcpy(params + kPartParamsIndex, &part, sizeof(part));
    if (!slot<int(__fastcall*)(void*, void*)>(physics, kPhysicsGetParams)(physics, params)) {
      break;
    }
    pose.append(reinterpret_cast<const char*>(params + kPartParamsTransform), kPartTransformSize);
  }
  return pose.size() > sizeof(kRagdollPoseTag) - 1 ? pose : std::string{};
}

bool valid_ragdoll_pose(const std::string& pose) {
  const std::size_t tag = sizeof(kRagdollPoseTag) - 1;
  return pose.size() > tag && pose.compare(0, tag, kRagdollPoseTag) == 0 &&
         (pose.size() - tag) % kPartTransformSize == 0;
}

// Returns the number of parts set.
unsigned apply_ragdoll_pose(void* entity, const std::string& pose) {
  void* physics = native_part_params_init ? articulated_physics(entity) : nullptr;
  if (!physics || !valid_ragdoll_pose(pose)) {
    return 0;
  }
  const std::size_t tag = sizeof(kRagdollPoseTag) - 1;
  const unsigned parts = unsigned((pose.size() - tag) / kPartTransformSize);
  unsigned set = 0;
  for (unsigned part = 0; part < parts; ++part) {
    alignas(16) std::uint8_t params[kPartParamsSize] = {};
    native_part_params_init(params);
    std::memcpy(params + kPartParamsIndex, &part, sizeof(part));
    std::memcpy(params + kPartParamsTransform, pose.data() + tag + part * kPartTransformSize,
                kPartTransformSize);
    if (slot<int(__fastcall*)(void*, void*, int)>(physics, kPhysicsSetParams)(physics, params, 0)) {
      ++set;
    }
  }
  return set;
}

// Whether the trampled replica of this player lies as a ragdoll this frame (engine thread).
// The owner's fall reaction (hit_reaction.xml, hit_type 3: fragment HitDeath, hasRagdoll) makes
// it a ragdoll right at the hit: its physicalization profile is "sleep" from the first frame and
// it drops within 0.7 s close to where it stood (live probe of the owner every 30 ms). The
// replica plays the reaction but its ragdoll procedural does not take effect (live video: it
// stayed upright in front of the horse), so the replica gets that same profile right after the
// hit (a script RagDollize 0.4 s later used another profile and fell elsewhere, live). Until then
// the replica follows its owner's replicated position; while it is a ragdoll, its physics moves
// it.
constexpr ULONGLONG kTrampleReactionRagdollMs = 30;
std::unordered_set<core::EntityId> trample_ragdolled;

bool trample_ragdoll_physics(core::EntityId player) {
  return trample_ragdolled.count(player) != 0;
}

// While the replica gets up it keeps the heading it lay with: its ragdoll lies with its own
// orientation, and turning it to its owner's at once spun it on the ground as the getting-up clip
// started (live video). It takes its owner's heading again when it stands.
constexpr ULONGLONG kTrampleGettingUpMs = 2500;
struct GettingUp {
  ULONGLONG until = 0;
  bool own_heading = false;
  core::Quat heading{};
};
std::unordered_map<core::EntityId, GettingUp> trample_getting_up;

// The heading (rotation about the up axis) of an entity matrix; false when it lies so that its
// forward axis is almost vertical.
bool heading_of(const float* m, core::Quat& heading) {
  const float fx = m[1];
  const float fy = m[5];
  if (fx * fx + fy * fy < 0.09f) {
    return false;
  }
  const float yaw = std::atan2(-fx, fy);
  heading = core::Quat{};
  heading.w = std::cos(yaw / 2);
  heading.z = std::sin(yaw / 2);
  return true;
}

// The turn about the up axis from heading `from` to heading `to` (both about the up axis), the
// shorter way.
core::Quat turn_between(const core::Quat& from, const core::Quat& to) {
  const float from_yaw = 2.0f * std::atan2(from.z, from.w);
  const float to_yaw = 2.0f * std::atan2(to.z, to.w);
  float turn = std::remainder(to_yaw - from_yaw, 2.0f * 3.14159265f);
  core::Quat result;
  result.z = std::sin(turn / 2);
  result.w = std::cos(turn / 2);
  return result;
}

bool replica_getting_up(core::EntityId player) {
  const auto it = trample_getting_up.find(player);
  return it != trample_getting_up.end() && GetTickCount64() < it->second.until;
}

const core::Quat& replica_heading(core::EntityId player, const core::Quat& owner) {
  auto it = trample_getting_up.find(player);
  if (it == trample_getting_up.end()) {
    return owner;
  }
  if (GetTickCount64() >= it->second.until) {
    trample_getting_up.erase(it);
    return owner;
  }
  return it->second.own_heading ? it->second.heading : owner;
}

// Distance of the replica from its owner's replicated position (diagnostics).
float owner_distance(void* entity, const core::Vec3& owner) {
  const float* tm = entity ? slot<const float*(__fastcall*)(void*)>(entity, 32)(entity) : nullptr;
  if (!tm) {
    return -1.0f;
  }
  const float dx = owner.x - tm[3];
  const float dy = owner.y - tm[7];
  const float dz = owner.z - tm[11];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool tick_trample_ragdoll(core::EntityId player, const std::optional<local::AnimationState>& clips,
                          std::uint64_t applied_body_sequence, void* actor, void* skeleton,
                          void* entity, const core::Vec3& owner) {
  const ULONGLONG now = GetTickCount64();
  if (trample_ragdoll_pending.erase(player)) {
    trample_ragdoll_started[player] = now;
    if (skeleton) {
      std::lock_guard<std::mutex> lock(trample_fall_mutex);
      trample_fall_skeletons.insert(skeleton);
    }
    log("COMBAT TRAMPLE replica fall held player=%u", player);
  }
  auto started = trample_ragdoll_started.find(player);
  if (started == trample_ragdoll_started.end()) {
    return false;
  }
  const ULONGLONG elapsed = now - started->second;
  if (skeleton && elapsed < 1200) {
    static ULONGLONG next_layers = 0;
    if (now >= next_layers) {
      next_layers = now + 50;
      int top[2] = {-2, -2};
      unsigned counts[2] = {0, 0};
      const unsigned layers[2] = {0, kTrampleFallLayer};
      for (unsigned i = 0; i < 2; ++i) {
        counts[i] = slot<unsigned(__fastcall*)(void*, unsigned)>(skeleton, 15)(skeleton, layers[i]);
        if (counts[i] && counts[i] <= 16) {
          auto raw = slot<const std::uint8_t*(__fastcall*)(void*, unsigned, unsigned)>(
              skeleton, 18)(skeleton, layers[i], counts[i] - 1);
          top[i] = *reinterpret_cast<const int*>(raw + 16);
        }
      }
      log("COMBAT TRAMPLE replica fall layers t=%llu layer0=%u/%d layer8=%u/%d",
          static_cast<unsigned long long>(elapsed), counts[0], top[0], counts[1], top[1]);
    }
  }
  if (elapsed >= kTrampleReactionRagdollMs && trample_ragdolled.insert(player).second) {
    const bool set = set_physics_profile(actor, PhysicsProfile::Sleep);
    log("COMBAT TRAMPLE replica ragdoll player=%u after=%llu profile=%u owner_distance=%.2f",
        player, static_cast<unsigned long long>(elapsed), unsigned(set),
        owner_distance(entity, owner));
  }
  bool owner_gets_up = false;
  if (elapsed >= kTrampleRagdollMinMs && clips) {
    const auto& body = (*clips)[0];
    owner_gets_up = body.clip >= 0 && body.sequence > applied_body_sequence;
  }
  if (elapsed < kTrampleRagdollMaxMs && !owner_gets_up) {
    return true;
  }
  const bool ran = set_physics_profile(actor, PhysicsProfile::Alive);
  trample_ragdoll_started.erase(started);
  trample_ragdolled.erase(player);
  // Its owner's animation manages it again right away: with the fall window still open the
  // replica's own idle (10670) replaced the owner's getting up 15 ms after it started (the
  // rider saw it get up in a T-pose).
  trample_fall_until.erase(player);
  {
    GettingUp getting_up;
    getting_up.until = now + kTrampleGettingUpMs;
    const float* tm = entity ? slot<const float*(__fastcall*)(void*)>(entity, 32)(entity) : nullptr;
    getting_up.own_heading = tm && heading_of(tm, getting_up.heading);
    trample_getting_up[player] = getting_up;
  }
  if (skeleton) {
    std::lock_guard<std::mutex> lock(trample_fall_mutex);
    trample_fall_skeletons.erase(skeleton);
  }
  log("COMBAT TRAMPLE replica ragdoll released player=%u after=%llu owner_clip=%u script=%u "
      "owner_distance=%.2f",
      player, static_cast<unsigned long long>(elapsed), unsigned(owner_gets_up), unsigned(ran),
      owner_distance(entity, owner));
  return false;
}

// A replica whose owner lies as a ragdoll for another reason than a trample of this client's horse
// (a knock-out) lies as one too: the owner's body plays no clips then, so the replica
// stood in a T-pose. The owner sends its ragdoll pose only while its physics is a ragdoll;
// the replica takes the ragdoll profile while the pose arrives and the living one when it stops.
std::unordered_set<core::EntityId> owner_ragdolled;
// The owner's ragdoll pose a replica last took, and the physics it was set on. A pose is set once,
// when it changes or the replica's physics is new: set again every frame, it fought the replica's
// own ragdoll between its owner's updates, and the body lying on the ground flickered between two
// poses (live video, a player standing at it).
std::unordered_map<core::EntityId, AppliedRagdollPose> applied_ragdoll_poses;

// The replicas whose owner stopped lying: their ragdoll turns into a living body in the frame after
// their owner's next base-layer clip (its getting up) went on them, or after kOwnerGetUpWaitMs
// without one (finish_owner_get_up). The owner's clip can come a frame after its ragdoll pose stops
// (62 ms later). The frame between lets the game pose the body by the clip: the blend from
// the ragdoll gives the entity the heading between the ragdoll's hips and the hips of the body's
// animated pose (the atan2 of joint 148's rotations), so the getting up's first
// frame must be that pose, as on the owner, whose clip starts before its body is living.
constexpr ULONGLONG kOwnerGetUpWaitMs = 500;
struct OwnerGetUp {
  ULONGLONG until = 0;
  std::uint64_t body_sequence = 0;
  bool clip_on = false;
};
std::unordered_map<core::EntityId, OwnerGetUp> owner_getting_up;

// A ragdoll takes clips while it is still one, as the owner's getting up starts its clip on it
// (which the getting up calls before its clip): the entity's character
// (entity slot 101) gets slots 18 and 40 with true. Without them the replica refused its owner's
// getting up until it was animated again (the clip came 500 ms late).
constexpr std::size_t kEntityCharacter = 101;
constexpr std::size_t kCharacterClipsOnRagdoll = 18;
constexpr std::size_t kCharacterClipsOnRagdollPose = 40;

void let_ragdoll_take_clips(void* entity) {
  using Character = void*(__fastcall*)(void*, unsigned);
  void* character = entity ? slot<Character>(entity, kEntityCharacter)(entity, 0) : nullptr;
  if (!character) {
    return;
  }
  using SetFlag = void(__fastcall*)(void*, bool);
  slot<SetFlag>(character, kCharacterClipsOnRagdoll)(character, true);
  slot<SetFlag>(character, kCharacterClipsOnRagdollPose)(character, true);
}

bool tick_owner_ragdoll(core::EntityId player, void* actor, bool owner_ragdoll,
                        std::uint64_t body_sequence, void* entity) {
  if (trample_ragdoll_started.count(player)) {
    return false;
  }
  // A frame that applied no clips of its owner's (finish_owner_get_up not called) still ends the
  // wait once it is over.
  finish_owner_get_up(player, actor, body_sequence);
  const bool lying = owner_ragdolled.count(player) != 0;
  if (owner_ragdoll == lying) {
    return lying;
  }
  applied_ragdoll_poses.erase(player);
  if (!owner_ragdoll) {
    owner_ragdolled.erase(player);
    owner_getting_up[player] = OwnerGetUp{GetTickCount64() + kOwnerGetUpWaitMs, body_sequence};
    let_ragdoll_take_clips(entity);
    // It gets up as its owner does, with its owner's heading (replica_getting_up).
    GettingUp getting_up;
    getting_up.until = GetTickCount64() + kTrampleGettingUpMs;
    trample_getting_up[player] = getting_up;
    log("REMOTE ragdoll player=%u lying=0: animated again after its owner's clips", player);
    return false;
  }
  const bool set = set_physics_profile(actor, PhysicsProfile::Sleep);
  owner_ragdolled.insert(player);
  log("REMOTE ragdoll player=%u lying=1 profile=%u", player, unsigned(set));
  return true;
}

void finish_owner_get_up(core::EntityId player, void* actor, std::uint64_t body_sequence) {
  const auto waiting = owner_getting_up.find(player);
  if (waiting == owner_getting_up.end()) {
    return;
  }
  auto& wait = waiting->second;
  // The frame the clip went on (the call after its owner's clips): the body takes its pose before
  // it is animated again, at the next frame's call (tick_owner_ragdoll).
  if (!wait.clip_on && body_sequence > wait.body_sequence) {
    wait.clip_on = true;
    return;
  }
  if (!wait.clip_on && GetTickCount64() < wait.until) {
    return;
  }
  const bool clip_on = wait.clip_on;
  owner_getting_up.erase(waiting);
  const bool set = set_physics_profile(actor, PhysicsProfile::Alive);
  log("REMOTE ragdoll player=%u animated again profile=%u owner_clip=%u", player, unsigned(set),
      unsigned(clip_on));
}

void trace_lying_body(const char* who, core::EntityId player, void* entity) {
  static const bool wanted = GetEnvironmentVariableW(L"KCDS_TEST_REPLICA_TRACE", nullptr, 0) > 0;
  if (!wanted || !entity) {
    return;
  }
  struct Trace {
    ULONGLONG next = 0;
    ULONGLONG until = 0;
    std::string pelvis;
  };
  static std::unordered_map<std::string, Trace> traces;
  auto& trace = traces[std::string(who) + std::to_string(player)];
  const ULONGLONG now = GetTickCount64();
  if (now < trace.next) {
    return;
  }
  trace.next = now + 50;
  namespace props = kcds::runtime::props::entity;
  if (trace.pelvis.empty()) {
    trace.pelvis = props::body_joint_name(entity, "Hips");
  }
  float hips[3]{};
  float turn[4]{};
  if (trace.pelvis.empty() || !props::joint_pose(entity, trace.pelvis.c_str(), hips, turn)) {
    return;
  }
  const float* tm = slot<const float*(__fastcall*)(void*)>(entity, 32)(entity);
  const float height = hips[2] - tm[11];
  if (height < 0.6f) {
    trace.until = now + 1500;
  }
  if (now >= trace.until) {
    return;
  }
  log("GETUP %s player=%u hips=%s height=%.2f forward=(%.2f %.2f %.2f) up=(%.2f %.2f %.2f) "
      "hips_q=(%.3f %.3f %.3f %.3f) hips_at=(%.2f %.2f %.2f) at=(%.2f %.2f %.2f) tick=%llu",
      who, player, trace.pelvis.c_str(), height, tm[1], tm[5], tm[9], tm[2], tm[6], tm[10], turn[0],
      turn[1], turn[2], turn[3], hips[0], hips[1], hips[2], tm[3], tm[7], tm[11], now);
}

// Test hook (KCDS_TEST_HOOKS only): the last trample's HitInfo, replayed on the replica on
// request (Local\KCD2S.TrampleReplay.<pid>) so its fall can be watched without riding into it.
HANDLE trample_replay_event = nullptr;
std::array<std::uint8_t, 160> last_trample_hit{};
core::EntityId last_trample_victim = 0;

// A trample needs the horse to move under its rider: the speed of its locomotion request (world
// metres per second, what the rider makes it do). The physics of a horse being mounted reported
// 2.8 m/s against a player standing next to it, who was knocked down by a horse that did not walk,
// and the mount it broke left the rider on its on-foot clips, standing on the horse.
constexpr float kTrampleMinHorseSpeed = 1.5f;
constexpr std::size_t kAnimatedEngineId = 48;

float local_horse_speed(unsigned engine_id) {
  std::lock_guard<std::shared_mutex> lock(motion_mutex);
  for (const auto& [skeleton, capture] : horse_captures) {
    if (!capture.authority || !capture.valid || !capture.animated) {
      continue;
    }
    const auto id = *reinterpret_cast<const unsigned*>(
        static_cast<const std::uint8_t*>(capture.animated) + kAnimatedEngineId);
    if (id == engine_id) {
      const core::Vec3& v = capture.motion.velocity;
      return std::sqrt(v.x * v.x + v.y * v.y);
    }
  }
  return 0.0f;
}

// A collision hit of the local rider's horse (either way) while the horse does not move under its
// rider: neither the trample nor the hit back on the horse is queued (that one broke the rider's
// mount, live).
bool slow_ridden_horse_collision(const std::uint8_t* hit) {
  const unsigned horse = local_ridden_horse_engine.load();
  if (!horse || *reinterpret_cast<const unsigned*>(hit + 24) != kCollisionHit) {
    return false;
  }
  const unsigned source = *reinterpret_cast<const unsigned*>(hit);
  const unsigned target = *reinterpret_cast<const unsigned*>(hit + 40);
  if (source != horse && target != horse) {
    return false;
  }
  const float horse_speed = local_horse_speed(horse);
  if (horse_speed >= kTrampleMinHorseSpeed) {
    return false;
  }
  log("COMBAT TRAMPLE ignored source=%u target=%u: the horse moves %.2f m/s", source, target,
      horse_speed);
  return true;
}

// Returns true when the hit was such a trample.
bool relay_trample(const std::uint8_t* hit) {
  const unsigned horse = local_ridden_horse_engine.load();
  if (!horse || *reinterpret_cast<const unsigned*>(hit + 24) != kCollisionHit ||
      *reinterpret_cast<const unsigned*>(hit) != horse) {
    return false;
  }
  const unsigned target = *reinterpret_cast<const unsigned*>(hit + 40);
  core::EntityId victim = 0;
  void* victim_actor = nullptr;
  {
    std::lock_guard<std::mutex> lock(combat_identity_mutex);
    for (const auto& entry : remote_combat_movements) {
      auto actor =
          *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(entry.second) + 504);
      if (actor && *reinterpret_cast<const unsigned*>(actor + 48) == target) {
        auto identity = remote_combat_souls.find(entry.first);
        if (identity != remote_combat_souls.end()) {
          victim = identity->second;
          victim_actor = actor;
        }
        break;
      }
    }
  }
  if (!victim) {
    return false;
  }
  const ULONGLONG now = GetTickCount64();
  auto last = trample_relayed_at.find(victim);
  if (last != trample_relayed_at.end() && now - last->second < kTrampleGapMs) {
    return true;
  }
  trample_relayed_at[victim] = now;
  trample_fall_until[victim] = now + kTrampleFallMs;
  trample_ragdoll_pending.insert(victim);
  clip_trace_until.store(now + kTrampleFallMs);
  std::memcpy(last_trample_hit.data(), hit, last_trample_hit.size());
  last_trample_victim = victim;
  integration::NativeReactionData data{};
  std::memcpy(data.data(), hit, 160);
  data = integration::portable_reaction_data(data);
  const bool collision = pack_trample_collision(data, victim_actor);
  bool sent;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    sent = client->peer().send_reaction(local_network_id.load(), victim, data, false);
  }
  float health_delta = 0;
  std::memcpy(&health_delta, hit + 44, 4);
  log("COMBAT TRAMPLE relayed source=%u target=%u sent=%u collision=%u horse_first=%u "
      "native_health_delta=%.3f",
      local_network_id.load(), victim, unsigned(sent), unsigned(collision),
      unsigned(data[kTrampleHorseFirstOffset]), health_delta);
  return true;
}

void tick_trample_replay() {
  if (!trample_replay_event || WaitForSingleObject(trample_replay_event, 0) != WAIT_OBJECT_0 ||
      !last_trample_victim) {
    return;
  }
  auto rules = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(get_services()) + 416);
  if (!rules) {
    return;
  }
  const ULONGLONG now = GetTickCount64();
  trample_fall_until[last_trample_victim] = now + kTrampleFallMs;
  trample_ragdoll_pending.insert(last_trample_victim);
  clip_trace_until.store(now + kTrampleFallMs);
  original_queue_native_hit(rules, last_trample_hit.data());
  log("COMBAT TRAMPLE TEST replayed the last trample on player=%u", last_trample_victim);
}

}  // namespace kcds::game_client
