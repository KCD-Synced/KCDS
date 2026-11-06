// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "replica_look.hpp"

#include "../../../Backend/integration/local/animation.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../look/look_pose_layer.hpp"
#include "../look/native_look.hpp"
#include "../ranged/aim.hpp"
#include "motion_capture.hpp"

#include <cmath>
#include <mutex>

namespace kcds::game_client {

// The owner's look (where its head turns) on its replica's native look IK; the look hooks keep
// the replica on it. Returns the replica's look and whether the owner's look was applied.
template <class OwnerState>
AppliedLook apply_remote_look(Remote& remote, const OwnerState& e, void* entity, void* movement,
                              bool guard_alignment) {
  AppliedLook applied;
  auto character = slot<void*(__fastcall*)(void*, unsigned)>(entity, 101)(entity, 0);
  applied.look = native_look(character);
  applied.target = std::get<core::Vec3>(e.fields[local::LookTarget]);
  applied.valid =
      std::get<bool>(e.fields[local::LookValid]) && finite_look(applied.target) && !guard_alignment;
  applied.enabled = std::get<bool>(e.fields[local::LookEnabled]);
  {
    std::lock_guard<std::shared_mutex> lock(motion_mutex);
    if (remote.look != applied.look) {
      remote_looks.erase(remote.look);
    }
    remote.look = applied.look;
    if (applied.look && applied.valid) {
      remote_looks[applied.look] = {movement, applied.target, applied.enabled};
    } else {
      remote_looks.erase(applied.look);
    }
  }
  if (!applied.look || !applied.valid) {
    return applied;
  }
  const auto tuning = std::get<core::Vec3>(e.fields[local::LookTuning]);
  if (std::isfinite(tuning.x) && std::isfinite(tuning.y) && tuning.x >= 0 && tuning.x <= 10 &&
      tuning.y >= 0 && tuning.y <= 10) {
    native_look_fade_in(applied.look, tuning.x);
    native_look_fade_out(applied.look, tuning.y);
  }
  original_look_target(applied.look, &applied.target);
  original_look_state(applied.look, applied.enabled ? 1 : 0);
  return applied;
}
// Every 3 s per replica: its owner's look and what the replica's native look IK makes of it in
// which posture. owner_animations: the owner's field "animations", for the clip on its
// look-pose layer.
void log_replica_look(core::EntityId id, const char* posture, const AppliedLook& applied,
                      void* skeleton, const std::string& owner_animations) {
  static std::unordered_map<core::EntityId, ULONGLONG> next_log;
  const ULONGLONG now_ms = GetTickCount64();
  auto& next = next_log[id];
  if (now_ms < next) {
    return;
  }
  next = now_ms + 3000;
  int owner_look_pose = -1;
  if (const auto clips = local::decode_animation(owner_animations)) {
    owner_look_pose = (*clips)[kcds::runtime::look::kLookPoseLayer].clip;
  }
  const NativeLookUse use = native_look_use(applied.look, skeleton);
  log("LOOK replica player=%u posture=%s owner valid=%u enabled=%u look_pose=%d replica state=%u "
      "layer=%u look_pose=%d poses=%u fade=%.2f",
      id, posture, unsigned(applied.valid), unsigned(applied.enabled), owner_look_pose,
      unsigned(use.state), use.layer, use.clip, use.poses, use.fade);
}
// The owner's look poses wait while the game mounts or dismounts the replica
// (look/look_pose_layer.hpp); logged once per sequence.
void note_waiting_look_pose(core::EntityId id, Remote& remote, const local::AnimationLayer& clip) {
  if (remote.waiting_look_pose == clip.sequence) {
    return;
  }
  remote.waiting_look_pose = clip.sequence;
  log("LOOK replica player=%u look_pose=%d sequence=%llu waits for the game's mount or dismount",
      id, clip.clip, clip.sequence);
}
// Where the owner's drawn weapon points on its replica's aim pose blender.
template <class OwnerState>
void apply_remote_aim(Remote& remote, const OwnerState& e, void* entity) {
  auto character = slot<void*(__fastcall*)(void*, unsigned)>(entity, 101)(entity, 0);
  void* aim = kcds::runtime::ranged::aim_of(native_pose(character));
  if (aim != remote.aim) {
    kcds::runtime::ranged::forget_replica_aim(remote.aim);
    remote.aim = aim;
  }
  kcds::runtime::ranged::AimState owner;
  owner.target = std::get<core::Vec3>(e.fields[local::AimTarget]);
  owner.enabled = std::get<bool>(e.fields[local::AimEnabled]);
  kcds::runtime::ranged::keep_replica_aim(aim, owner);
}
// The owner's view on its replica: the actor's view rotation (player vtable +448
// the game's own TeleportPlayer sets it after placing the player). The replica's
// movement controller turns the body to it and the aim pose follows its pitch.
void apply_remote_view(void* actor, const core::Vec3& direction) {
  const float length =
      std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
  if (!actor || !(length > 0.5f) || !std::isfinite(length)) {
    return;
  }
  const float yaw = std::atan2(-direction.x, direction.y);
  const float pitch = std::asin(std::clamp(direction.z / length, -1.0f, 1.0f));
  const float cz = std::cos(yaw * 0.5f);
  const float sz = std::sin(yaw * 0.5f);
  const float cx = std::cos(pitch * 0.5f);
  const float sx = std::sin(pitch * 0.5f);
  core::Quat view;
  view.x = cz * sx;
  view.y = sz * sx;
  view.z = sz * cx;
  view.w = cz * cx;
  slot<void(__fastcall*)(void*, const core::Quat*)>(actor, 56)(actor, &view);
}

// game_client.cpp applies them to the replicated state it shows.
template AppliedLook apply_remote_look<RemoteState>(Remote& remote, const RemoteState& e,
                                                    void* entity, void* movement,
                                                    bool guard_alignment);
template void apply_remote_aim<RemoteState>(Remote& remote, const RemoteState& e, void* entity);

}  // namespace kcds::game_client
