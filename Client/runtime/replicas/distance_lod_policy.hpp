// Distance LOD owns only its own default-ratio override. Native setters may queue;
// observe their result before releasing ownership or applying another override.
#pragma once

#include <cmath>
#include <optional>

namespace kcds::runtime::replicas {

class DistanceLodPolicy {
 public:
  static constexpr int kNativeRatio = 100;
  static constexpr int kDistantRatio = 108;
  static constexpr float kEnterSquared = 80.0f * 80.0f;
  static constexpr float kLeaveSquared = 60.0f * 60.0f;

  // A returned value is one request to the native SetLodRatio method. The native
  // queue preserves request order, including a restore before an apply is observed.
  std::optional<int> update(const void* node, int current, float distance_squared, bool eligible) {
    if (node != node_) {
      node_ = node;
      phase_ = Phase::idle;
    }
    if (!node || phase_ == Phase::yielded) {
      return std::nullopt;
    }
    if (phase_ == Phase::applied && current != kDistantRatio) {
      // Another writer changed the observed ratio. Do not restore over that choice
      // or reacquire this render node later in the same native lifetime.
      phase_ = Phase::yielded;
      return std::nullopt;
    }
    if (phase_ != Phase::idle && current != kNativeRatio && current != kDistantRatio) {
      phase_ = Phase::yielded;
      return std::nullopt;
    }
    if (phase_ == Phase::applying && current == kDistantRatio) {
      phase_ = Phase::applied;
    }
    if (phase_ == Phase::restoring) {
      if (current != kNativeRatio) {
        return std::nullopt;
      }
      phase_ = Phase::idle;
    }
    eligible = eligible && std::isfinite(distance_squared) && distance_squared >= 0;
    if (phase_ == Phase::applying || phase_ == Phase::applied) {
      if (!eligible || distance_squared <= kLeaveSquared) {
        phase_ = Phase::restoring;
        return kNativeRatio;
      }
      return std::nullopt;
    }
    if (eligible && distance_squared >= kEnterSquared && current == kNativeRatio) {
      phase_ = Phase::applying;
      return kDistantRatio;
    }
    return std::nullopt;
  }

  bool owns_override() const {
    return phase_ == Phase::applying || phase_ == Phase::applied || phase_ == Phase::restoring;
  }

 private:
  enum class Phase { idle, applying, applied, restoring, yielded };
  const void* node_ = nullptr;
  Phase phase_ = Phase::idle;
};

}  // namespace kcds::runtime::replicas
