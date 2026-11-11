#include "../core/client_internal.hpp"

#include "distance_lod.hpp"

#include <cmath>
#include <unordered_set>

#include "../core/frame.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../props/prop_entity.hpp"
#include "../riding/native_horses.hpp"
#include "kcd2s/integration/horse.hpp"
#include "kcd2s/integration/player_stats.hpp"
#include "kcd2s/integration/posture.hpp"
#include "remote.hpp"

namespace kcds::game_client {
namespace {

using GetProxy = void*(__fastcall*)(void*, int);
using GetRenderNode = void*(__fastcall*)(void*);
using SetLodRatio = void(__fastcall*)(void*, int);
GetProxy get_proxy = nullptr;
GetRenderNode get_render_node = nullptr;
SetLodRatio set_lod_ratio = nullptr;
constexpr const char* kSetting = "KCDS_REMOTE_DISTANCE_LOD";

bool initialize() {
  char setting[8]{};
  const auto length = GetEnvironmentVariableA(kSetting, setting, sizeof(setting));
  if (length == 0 || (length == 1 && setting[0] == '0')) {
    return false;
  }
  if (length != 1 || setting[0] != '1') {
    log("REMOTE distance LOD invalid %s; expected 0 or 1, feature disabled", kSetting);
    return false;
  }
  // WHGame 1.5.5: CEntity::GetProxy(0), CRenderProxy's secondary-interface
  // GetRenderNode, and its main-interface SetLodRatio. Validate all three objects'
  // virtual targets before using them; another render-node class is left untouched.
  get_proxy = reinterpret_cast<GetProxy>(resolve_unique(
      "DistanceLodGetProxy",
      "4C 8B 81 A8 00 00 00 45 33 D2 49 8B C8 4D 8B 48 08 EB 13 41 39 51 20 49 8D 41 10 49 "
      "0F 43 C1 49 0F 43 C9 4C 8B 08 45 38 51 19 74 E7 49 3B C8 74 10"));
  get_render_node =
      reinterpret_cast<GetRenderNode>(resolve_unique("DistanceLodGetRenderNode", "48 8D 41 B0 C3"));
  // The fixed relative call identifies this queue command among otherwise identical
  // wrappers. A changed binary must resolve uniquely again before any LOD write.
  set_lod_ratio = reinterpret_cast<SetLodRatio>(resolve_unique(
      "DistanceLodSetRatio",
      "48 8B C4 48 89 58 18 48 89 70 20 57 48 83 EC 30 48 8B 3D ?? ?? ?? ?? 48 8B F1 48 89 "
      "48 E8 48 8B CF 89 50 F0 48 8D 50 E8 C6 40 10 00 E8 53 BA 80 01 84 C0 75 33 48 8D 4F "
      "60 E8 ?? ?? ?? ?? 48 8D 46 78 48 8D 4F 70"));
  if (!get_proxy || !get_render_node || !set_lod_ratio) {
    log("REMOTE distance LOD signatures unavailable; feature disabled");
    return false;
  }
  log("REMOTE distance LOD enabled; idle on-foot enter=80m leave=60m ratio=108");
  return true;
}

void* render_node(void* entity) {
  if (slot<GetProxy>(entity, 74) != get_proxy) {
    return nullptr;
  }
  void* proxy = get_proxy(entity, 0);
  if (!proxy || slot<GetRenderNode>(proxy, 26) != get_render_node) {
    return nullptr;
  }
  void* node = get_render_node(proxy);
  return node && slot<SetLodRatio>(node, 28) == set_lod_ratio ? node : nullptr;
}

float squared_length(const core::Vec3& vector) {
  return vector.x * vector.x + vector.y * vector.y + vector.z * vector.z;
}

bool idle_on_foot(const Remote& remote, const kcd2s::replication::Mirror::Entity& state,
                  bool has_rider_pair, bool must_stand_still = true) {
  const auto& fields = state.fields;
  if (fields.size() <= local::StationAction || !remote.actor || !remote.skeleton ||
      remote.wake_presentation_held || has_rider_pair || !native_human_mounted ||
      native_human_mounted(remote.actor) || !std::get<bool>(fields[local::PoseValid]) ||
      !std::get<bool>(fields[local::MotionValid]) ||
      (std::get<std::int64_t>(fields[local::MotionFlags]) & 2) != 0 ||
      !std::get<bool>(fields[local::BodyVisible]) || std::get<bool>(fields[local::AimEnabled]) ||
      std::get<std::int64_t>(fields[local::HeldProp]) != 0 ||
      !std::get<std::string>(fields[local::NativeRiderState]).empty() ||
      !std::get<std::string>(fields[local::RagdollPose]).empty() ||
      !std::get<std::string>(fields[local::Seats]).empty() ||
      !std::get<std::string>(fields[local::Carry]).empty() ||
      !std::get<std::string>(fields[local::StationAction]).empty()) {
    return false;
  }
  // Every body that reports vitals has a wake scope, so what counts is whether it is down: getting
  // up shows in wake_presentation_held above.
  kcd2s::integration::PlayerVitals vitals;
  if (fields.size() > local::Vitals &&
      kcd2s::integration::parse_vitals_field(std::get<std::string>(fields[local::Vitals]),
                                             vitals) &&
      (vitals.values[0] <= 0.0f || vitals.values[6] <= 0.0f)) {
    return false;
  }
  kcd2s::integration::PlayerPosture posture;
  // The engine adapter publishes Posture and motion velocity; legacy Grounded/Gait
  // fields are used by old diagnostic peers and are not an idle signal for real players.
  if (!kcd2s::integration::parse_posture_field(std::get<std::int64_t>(fields[local::Posture]),
                                               posture) ||
      posture.in_air || posture.crouched || posture.guard || posture.raised || posture.on_ladder ||
      posture.fainted || posture.combat_zone >= 0) {
    return false;
  }
  const float speed = squared_length(std::get<core::Vec3>(fields[local::Velocity]));
  const float requested_speed = squared_length(std::get<core::Vec3>(fields[local::MotionVelocity]));
  if (must_stand_still && (!std::isfinite(speed) || !std::isfinite(requested_speed) ||
                           speed > 0.01f || requested_speed > 0.01f)) {
    return false;
  }
  void* movement = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(remote.actor) + 632);
  // Existing combat contract: 28 is no native action. Both owner posture and the
  // replica's current native action must be idle; stale owner state cannot hide a fight.
  return movement && native_action_type(movement) == 28;
}

}  // namespace

// Full updates a resting body skips in a row. Anything new from its owner ends the rest at once.
constexpr unsigned kRestFrames = 7;

// A calm body off screen takes a full update only every few frames, by its id, so a crowd's bodies
// do not all come in the same frame. Close to the camera it always does: it may walk into view.
// Only a body that stands rests then; one that walks is still put on its path every frame (a light
// frame), or it went on its own movement, fell 30 cm behind and jumped there every sixth frame.
constexpr unsigned kOffScreenEvery = 6;
constexpr float kOffScreenFrom = 6.0f;
// The widest screen the view cone must cover, so a body in a corner of it never rests.
constexpr float kWidestAspect = 21.0f / 9.0f;
static std::uint32_t rest_frames = 0;
static float off_screen_cosine = 1.0f;

void replica_rest_new_frame() {
  ++rest_frames;
  if (!view_matrix_valid) {
    return;
  }
  // The half angle from the view's centre to the screen's corner.
  const float tangent = std::tan(view_fov * 0.5f);
  const float corner_squared = tangent * tangent * (1.0f + kWidestAspect * kWidestAspect);
  off_screen_cosine = 1.0f / std::sqrt(1.0f + corner_squared);
}

bool replica_off_screen(const float body[3]) {
  if (!view_matrix_valid) {
    return false;
  }
  const float to_body[3] = {body[0] - view_matrix[3], body[1] - view_matrix[7],
                            body[2] - view_matrix[11]};
  const float distance_sq =
      to_body[0] * to_body[0] + to_body[1] * to_body[1] + to_body[2] * to_body[2];
  if (distance_sq < kOffScreenFrom * kOffScreenFrom) {
    return false;
  }
  // The camera looks along the matrix's second column.
  const float along =
      to_body[0] * view_matrix[1] + to_body[1] * view_matrix[5] + to_body[2] * view_matrix[9];
  return along < off_screen_cosine * std::sqrt(distance_sq);
}

bool replica_can_rest(core::EntityId id, Remote& remote,
                      const kcd2s::replication::Mirror::Entity& latest, bool shown_settled,
                      const float body[3]) {
  const bool changed = latest.revision != remote.rest_revision;
  if (changed) {
    remote.rest_revision = latest.revision;
    remote.rest_idle = idle_on_foot(remote, latest, false);
    remote.rest_calm = idle_on_foot(remote, latest, false, false);
    remote.rested_frames = 0;
  }
  if (remote.rest_idle && (rest_frames + id) % kOffScreenEvery != 0 && replica_off_screen(body)) {
    return true;
  }
  if (changed) {
    return false;
  }
  if (!remote.rest_idle || !shown_settled || remote.rested_frames >= kRestFrames) {
    remote.rested_frames = 0;
    return false;
  }
  ++remote.rested_frames;
  return true;
}

// In metres: closer than this a calm body takes a new state from its owner at once.
constexpr float kLightNear = 8.0f;
// Further away it takes one in every this many frames.
constexpr unsigned kLightTurn = 3;
// And a full update at least this often, changed or not.
constexpr unsigned kFullEvery = 12;

bool replica_turn(core::EntityId id, unsigned every) {
  return (rest_frames + id) % every == 0;
}

float replica_camera_distance_sq(const float body[3]) {
  if (!view_matrix_valid) {
    return 0.0f;
  }
  const float dx = body[0] - view_matrix[3];
  const float dy = body[1] - view_matrix[7];
  const float dz = body[2] - view_matrix[11];
  return dx * dx + dy * dy + dz * dz;
}

bool replica_light_frame(core::EntityId id, Remote& remote,
                         const kcd2s::replication::Mirror::Entity& latest, const float body[3]) {
  if (latest.revision != remote.light_revision) {
    remote.light_revision = latest.revision;
    remote.light_pending = true;
  }
  if (!remote.rest_calm) {
    remote.light_pending = false;
    return false;
  }
  const bool close = replica_camera_distance_sq(body) < kLightNear * kLightNear;
  // Off screen a walking body's new state waits as long as a resting body's would.
  const bool turn = replica_off_screen(body) ? replica_turn(id, kOffScreenEvery)
                                             : close || replica_turn(id, kLightTurn);
  const bool due = remote.light_pending && turn;
  if (due || replica_turn(id, kFullEvery)) {
    remote.light_pending = false;
    return false;
  }
  return true;
}

void tick_remote_distance_lod(const Frame& frame) {
  static const bool enabled = initialize();
  if (!enabled) {
    return;
  }
  float viewer[3]{};
  kcds::runtime::props::entity::world_position(frame.local_entity, viewer);
  std::unordered_set<core::EntityId> riders;
  // One pass per engine frame, never a scan from a native animation/movement callback.
  for (const auto& [id, horse] : native_horses) {
    if (horse.replica_rider) {
      riders.insert(horse.replica_rider);
    }
  }
  // A mirror can already assign a rider while its native horse is waiting for capacity.
  // Protect that pair before construction as well as during the native dismount tail.
  for (const auto& [id, state] : frame.entities) {
    if (state.type == core::type_id("NetworkHorse") &&
        state.fields.size() > kcd2s::integration::HorseRider) {
      const auto* rider = std::get_if<std::int64_t>(&state.fields[kcd2s::integration::HorseRider]);
      if (rider && *rider > 0) {
        riders.insert(core::EntityId(*rider));
      }
    }
  }
  std::size_t eligible_count = 0;
  std::size_t applied_count = 0;
  std::size_t pending_count = 0;
  std::size_t unsupported_count = 0;
  static std::uint64_t requests = 0;
  for (auto& [id, remote] : remotes) {
    void* entity = get_entity(frame.entity_system, remote.engine_id);
    void* node = entity ? render_node(entity) : nullptr;
    if (!node) {
      remote.distance_lod.update(nullptr, 0, 0, false);
      ++unsupported_count;
      continue;
    }
    const auto latest = frame.entities.find(id);
    const bool eligible = latest != frame.entities.end() &&
                          idle_on_foot(remote, latest->second, riders.count(id) != 0);
    eligible_count += eligible;
    float position[3]{};
    kcds::runtime::props::entity::world_position(entity, position);
    const core::Vec3 offset{position[0] - viewer[0], position[1] - viewer[1],
                            position[2] - viewer[2]};
    // The native script getter reads this byte at +78. Read only:
    // SetLodRatio1808F092C owns synchronization, clamping, and child-node propagation.
    const int current = *(static_cast<const std::uint8_t*>(node) + 78);
    if (const auto ratio =
            remote.distance_lod.update(node, current, squared_length(offset), eligible)) {
      set_lod_ratio(node, *ratio);
      ++requests;
    }
    if (remote.distance_lod.owns_override()) {
      if (current == kcds::runtime::replicas::DistanceLodPolicy::kDistantRatio) {
        ++applied_count;
      } else {
        ++pending_count;
      }
    }
  }
  static double next_report = 0;
  if (frame.now >= next_report) {
    next_report = frame.now + 5;
    log("REMOTE distance LOD replicas=%zu idle=%zu ratio108=%zu pending=%zu unavailable=%zu "
        "requests=%llu",
        remotes.size(), eligible_count, applied_count, pending_count, unsupported_count, requests);
  }
}

}  // namespace kcds::game_client
