// What the client captures of bodies' motion and animation, for the local player and the horses
// it simulates, and what it plays back on replicas (the types split out of
// game_client.cpp). The maps are guarded by motion_mutex.
#pragma once

#include <shared_mutex>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "motion_index.hpp"

#include "../../../Backend/integration/local/animation.hpp"
#include "../../../Backend/integration/local/runtime.hpp"

namespace kcds::game_client {

namespace core = kcd2s::core;
namespace local = kcd2s::local;
struct NativeMovementRequest {
  std::uint32_t type{};
  core::Vec3 velocity{};
  core::Quat rotation{};
  float unused[7]{};
  std::uint8_t flags[4]{};
  std::uint8_t allow_strafe{};
  std::uint8_t padding[3]{};
  float prediction{};
  std::uint8_t jumping{};
  std::uint8_t tail[3]{};
};
static_assert(sizeof(NativeMovementRequest) == 76);
using MovementRequest = void*(__fastcall*)(void*, const NativeMovementRequest*);
using MotionParam = void(__fastcall*)(void*, int, float);
using ClearAnimation = std::int64_t(__fastcall*)(void*, unsigned);
using StartAnimation = char(__fastcall*)(void*, int, const void*);
struct RemoteInputs {
  void* animated{};
  void* movement{};
  NativeMovementRequest motion{};
  // The replica pass (replica_passes) that turned a player replica's `motion` toward its owner's
  // heading: the turn counts in the game update after that pass only. 0 (an NPC's or a horse's
  // request) keeps the turn every frame.
  std::uint64_t turn_pass = 0;
  bool valid{};
  float params[18]{};
  std::uint32_t mask{};
  local::AnimationState animations{};
};
struct HorseCapture {
  void* animated{};
  bool authority{};
  NativeMovementRequest motion{};
  bool valid{};
  float params[18]{};
  std::uint32_t mask{};
  local::AnimationState animations{};
};

// The motion params and animations, keyed by skeleton.
extern std::shared_mutex motion_mutex;
extern std::unordered_map<void*, local::AnimationStopFence> animation_stop_fences;
// The horses this client simulates, and the replicas' inputs (both under motion_mutex).
extern std::unordered_map<void*, HorseCapture> horse_captures;
extern kcds::runtime::replicas::MotionIndex<RemoteInputs> remote_inputs;
// The replica passes so far, one per frame (tick_replicas).
extern std::atomic<std::uint64_t> replica_passes;

// The game's movement request, motion params and animation start/clear (hooked: the originals).
extern MovementRequest original_movement_request;
extern MotionParam original_motion_param;
extern ClearAnimation original_clear_animation;
extern StartAnimation original_start_animation;

// Takes the clips a skeleton already plays into its captured animations.
void capture_existing_animations(void* skeleton, bool current_progress = false);

}  // namespace kcds::game_client
