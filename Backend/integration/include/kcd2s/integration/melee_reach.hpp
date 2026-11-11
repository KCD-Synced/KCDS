#pragma once
// Lag compensation for melee: the server keeps where each player stood over the last
// kPoseHistorySeconds and accepts a contact when the pair stood within kMeleeReach of each other at
// some moment of that window. An attacker's game shows the victim as it was a round trip and the
// replica delay ago, so a contact on what that game showed is fair even when the victim has moved
// on since; a contact on someone who was never near is refused with the distance as its reason.
// Portable, for the unit tests.
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>

#include "kcd2s/core/math.hpp"

namespace kcd2s::integration {

// How far back the server looks, and the longest reach of a blow: the longest weapon, a lunge and
// both bodies' radii, with a margin for the owner's own position reports.
inline constexpr double kPoseHistorySeconds = 0.5;
inline constexpr float kMeleeReach = 5.0f;

// What the server does with a contact out of reach: nothing, log it, or log it and drop it.
enum class MeleeReachMode { Off, Observe, Enforce };

// Where one player stood, sampled every server tick: a ring of the newest kPoseSamples samples.
class PoseHistory {
 public:
  static constexpr std::size_t kPoseSamples = 64;

  void record(double now, const core::Vec3& position) {
    samples_[next_] = {now, position};
    next_ = (next_ + 1) % kPoseSamples;
    if (count_ < kPoseSamples) {
      ++count_;
    }
  }

  void clear() {
    count_ = 0;
    next_ = 0;
  }

  std::size_t size() const {
    return count_;
  }

  // Calls `visit(position)` for each sample from `since` on, newest first.
  template <typename Visit>
  void each_since(double since, Visit&& visit) const {
    for (std::size_t i = 0; i < count_; ++i) {
      const Sample& sample = samples_[(next_ + kPoseSamples - 1 - i) % kPoseSamples];
      if (sample.at < since) {
        break;
      }
      visit(sample.position);
    }
  }

 private:
  struct Sample {
    double at = 0;
    core::Vec3 position{};
  };
  std::array<Sample, kPoseSamples> samples_{};
  std::size_t next_ = 0;
  std::size_t count_ = 0;
};

struct MeleeReachVerdict {
  bool ok = true;
  float distance = 0.0f;  // the closest the pair came in the window
  std::string reason;     // empty when ok
};

// Whether a and b stood within kMeleeReach of each other at any two moments of the window ending
// at `now` (each one's own report and the other's as the reporting game showed it). Without a
// sample of either the contact is not judged.
inline MeleeReachVerdict check_melee_reach(const PoseHistory& a, const PoseHistory& b, double now) {
  MeleeReachVerdict verdict;
  const double since = now - kPoseHistorySeconds;
  float closest = -1.0f;
  a.each_since(since, [&](const core::Vec3& from) {
    b.each_since(since, [&](const core::Vec3& to) {
      const float dx = from.x - to.x;
      const float dy = from.y - to.y;
      const float dz = from.z - to.z;
      const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
      if (closest < 0.0f || distance < closest) {
        closest = distance;
      }
    });
  });
  if (closest < 0.0f) {
    return verdict;
  }
  verdict.distance = closest;
  if (closest > kMeleeReach) {
    char text[96];
    std::snprintf(text, sizeof(text), "distance %.1f m > reach %.1f m in the last %.1f s", closest,
                  kMeleeReach, kPoseHistorySeconds);
    verdict.ok = false;
    verdict.reason = text;
  }
  return verdict;
}

}  // namespace kcd2s::integration
