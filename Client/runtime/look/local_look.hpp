// The local player's look: the field "gaze" and its look log.
#pragma once

#include <cstdint>

#include "kcd2s/core/math.hpp"

namespace kcds::game_client {

kcd2s::core::Vec3 tick_look_direction(kcd2s::core::Vec3 look_target, bool look_valid);
void log_local_look(void* look, void* skeleton, bool valid, bool enabled, std::int64_t posture);

// What frame() reads of the local player's look each frame: the native look
// (CLookIK) and aim, the look's target, tuning and state.
struct LocalLookReading {
  void* local_look = nullptr;
  void* local_aim = nullptr;
  kcd2s::core::Vec3 look_target{};
  kcd2s::core::Vec3 look_tuning{};
  bool look_enabled = false;
  bool look_valid = false;
};
// Called by frame() every frame; also keeps the local skeleton (local_skeleton).
void read_local_look(void* local_entity, LocalLookReading& reading);

}  // namespace kcds::game_client
