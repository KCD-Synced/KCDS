#pragma once
// The server's check of a player's movement (validation with reasons). A player's client owns its
// position; the server compares each new position with the last one over the owner's own clock
// (MotionClock, milliseconds) and says why a step is impossible, such as
// "speed 14.2 m/s > max 10.0 (on foot)", so an admin can tell a cheat from a false detection.
// A server teleport or a new forced-state epoch starts the check again. Portable, for the unit
// tests.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "kcd2s/core/math.hpp"

namespace kcd2s::integration {

// The fastest a player moves on foot (a sprint) and on a horse (a gallop), metres a second in the
// horizontal plane, with a margin for the owner's clock and frame steps; and the fastest it rises.
inline constexpr float kMaxFootSpeed = 10.0f;
inline constexpr float kMaxMountedSpeed = 22.0f;
inline constexpr float kMaxRiseSpeed = 12.0f;
// Steps shorter than this are judged together with the next ones (a jitter of the owner's clock).
inline constexpr double kMinCheckSeconds = 0.1;
// A larger gap of the owner's clock (a pause, a loading screen) starts the check again.
inline constexpr double kMaxCheckSeconds = 5.0;

struct MovementCheck {
  bool started = false;
  core::Vec3 position{};
  std::int64_t clock_ms = 0;
  std::uint8_t epoch = 0;
  // The last position a step was accepted to (where enforcement puts a player back).
  core::Vec3 accepted{};
};

struct MovementVerdict {
  bool ok = true;
  float speed = 0.0f;  // horizontal, or rising when that was the reason
  float limit = 0.0f;
  std::string reason;     // empty when ok
  core::Vec3 accepted{};  // the last accepted position, when refused
};

// What the server does with a refused step: nothing, log it, or log it and put the player back.
enum class MovementCheckMode { Off, Observe, Enforce };

// A refused step of a player entity.
struct MovementRefusal {
  std::uint32_t player = 0;
  MovementVerdict verdict;
};

// Judges the move to `position` at the owner's `clock_ms`. `mounted`: the player rides. Every
// judged step becomes the next one's start, a refused one too: a single jump (the game's own fast
// travel, a cutscene) is reported once, not with every step after it.
inline MovementVerdict check_movement(MovementCheck& check, const core::Vec3& position,
                                      std::int64_t clock_ms, std::uint8_t epoch, bool mounted) {
  MovementVerdict verdict;
  if (!check.started || epoch != check.epoch) {
    check = {true, position, clock_ms, epoch, position};
    return verdict;
  }
  const double seconds = double(clock_ms - check.clock_ms) / 1000.0;
  if (seconds < 0.0 || seconds > kMaxCheckSeconds) {
    check = {true, position, clock_ms, epoch, position};
    return verdict;
  }
  if (seconds < kMinCheckSeconds) {
    return verdict;
  }
  const float dx = position.x - check.position.x;
  const float dy = position.y - check.position.y;
  const float rise = position.z - check.position.z;
  const float speed = float(std::sqrt(dx * dx + dy * dy) / seconds);
  const float rising = float(rise / seconds);
  const float limit = mounted ? kMaxMountedSpeed : kMaxFootSpeed;
  check.position = position;
  check.clock_ms = clock_ms;
  char text[96];
  if (speed > limit) {
    std::snprintf(text, sizeof(text), "speed %.1f m/s > max %.1f (%s)", speed, limit,
                  mounted ? "mounted" : "on foot");
    verdict = {false, speed, limit, text, check.accepted};
    return verdict;
  }
  if (rising > kMaxRiseSpeed) {
    std::snprintf(text, sizeof(text), "rising %.1f m/s > max %.1f", rising, kMaxRiseSpeed);
    verdict = {false, rising, kMaxRiseSpeed, text, check.accepted};
    return verdict;
  }
  check.accepted = position;
  verdict.speed = speed;
  verdict.limit = limit;
  return verdict;
}

}  // namespace kcd2s::integration
