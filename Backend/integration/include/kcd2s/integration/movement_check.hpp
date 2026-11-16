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
  core::Vec3 to{};        // where the refused step ended
};

// A fast travel the player began: their game reports its target when the map's travel starts, and
// then carries the body along the roads faster than anyone walks. Its refused steps are logged
// but count for no strike, while the travel lasts and while they stay on its way.
struct FastTravel {
  bool active = false;
  double since = 0.0;
  core::Vec3 from{};
  core::Vec3 to{};
};

// How long a fast travel may take, in seconds.
inline constexpr double kFastTravelSeconds = 180.0;
// Its way: inside the ellipse around its start and its target whose distances add up to this
// many times the straight way plus kFastTravelSlack metres (the roads wind).
inline constexpr float kFastTravelDetour = 1.5f;
inline constexpr float kFastTravelSlack = 300.0f;

// Whether a refused step that ended `at` (at `now`) belongs to the fast travel.
inline bool on_fast_travel(const FastTravel& travel, const core::Vec3& at, double now) {
  if (!travel.active || now - travel.since < 0.0 || now - travel.since > kFastTravelSeconds) {
    return false;
  }
  const auto flat = [](const core::Vec3& a, const core::Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
  };
  const float straight = flat(travel.from, travel.to);
  const float way = flat(travel.from, at) + flat(at, travel.to);
  return way <= straight * kFastTravelDetour + kFastTravelSlack;
}

// A boarding by teleport the server ordered (Cart boardMode 'teleport'): the player's game puts
// them at their seat's climb start at once, a jump the check refuses. Its refused steps that end
// near the cart, for a few seconds after the order, are logged but count for no strike: three of
// them (getting on and off again) put the player back in the middle of their climb.
struct Boarding {
  bool active = false;
  // When the order went out.
  double since = 0.0;
  core::Vec3 cart{};
};

inline constexpr double kBoardingSeconds = 6.0;
inline constexpr float kBoardingReach = 10.0f;

// Whether a refused step that ended `at` (at `now`) is the boarding's jump.
inline bool on_boarding(Boarding& boarding, const core::Vec3& at, double now) {
  if (!boarding.active) {
    return false;
  }
  if (now - boarding.since < -1.0 || now - boarding.since > kBoardingSeconds) {
    boarding.active = false;
    return false;
  }
  const float dx = at.x - boarding.cart.x;
  const float dy = at.y - boarding.cart.y;
  return dx * dx + dy * dy <= kBoardingReach * kBoardingReach;
}

// What the server does with a refused step: nothing, log it, or log it and put the player back.
enum class MovementCheckMode { Off, Observe, Enforce };

// Enforcement puts a player back only after kStrikesToEnforce refused steps within
// kStrikeWindowSeconds: a single impossible step (a hitch of the game, a frame the owner's clock
// skipped) is logged but leaves the player where they are; a player who keeps moving impossibly
// is put back.
inline constexpr std::size_t kStrikesToEnforce = 3;
inline constexpr double kStrikeWindowSeconds = 10.0;

class MovementStrikes {
 public:
  // Counts a refused step at `now`; true when it is the one that puts the player back (the count
  // then starts again).
  bool strike(double now) {
    std::size_t kept = 0;
    for (std::size_t i = 0; i < count_; ++i) {
      if (now - at_[i] < kStrikeWindowSeconds) {
        at_[kept++] = at_[i];
      }
    }
    count_ = kept;
    at_[count_++] = now;
    if (count_ >= kStrikesToEnforce) {
      count_ = 0;
      return true;
    }
    return false;
  }

 private:
  double at_[kStrikesToEnforce] = {};
  std::size_t count_ = 0;
};

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
    verdict = {false, speed, limit, text, check.accepted, position};
    return verdict;
  }
  if (rising > kMaxRiseSpeed) {
    std::snprintf(text, sizeof(text), "rising %.1f m/s > max %.1f", rising, kMaxRiseSpeed);
    verdict = {false, rising, kMaxRiseSpeed, text, check.accepted, position};
    return verdict;
  }
  check.accepted = position;
  verdict.speed = speed;
  verdict.limit = limit;
  return verdict;
}

}  // namespace kcd2s::integration
