// Bound repeated native body reports without changing their wire format or authority fences.
#pragma once

#include <cmath>
#include <optional>

#include "horse.hpp"

namespace kcd2s::integration {

inline constexpr double kBodyMotionPublicationSeconds = 0.05;
inline constexpr double kBodyIdlePublicationSeconds = 0.5;
inline constexpr double kBodyCriticalRepeatSeconds = 0.2;
inline constexpr double kBodyActiveProgressTailSeconds = 4;

class BodyStatePublication {
 public:
  bool publish(const BodyState& state, double now, bool limit_motion = true,
               bool active_progress = false) {
    const auto animations = local::decode_animation(state.animations);
    if (!animations || !std::isfinite(now)) {
      return false;
    }
    if (active_progress || (state.motion_flags & kBodyFighting) != 0) {
      active_progress_until_ = now + kBodyActiveProgressTailSeconds;
    }
    const bool critical = !previous_ || previous_->revision != state.revision ||
                          previous_->epoch != state.epoch || previous_->source != state.source ||
                          previous_->motion_valid != state.motion_valid ||
                          previous_->motion_type != state.motion_type ||
                          previous_->motion_flags != state.motion_flags ||
                          previous_->mask != state.mask || !same_starts(*animations);
    const double elapsed = now - published_at_;
    const bool retry_critical = limit_motion && now - critical_at_ < kBodyCriticalRepeatSeconds &&
                                elapsed + 1e-8 >= kBodyMotionPublicationSeconds;
    // Mounted/fighting bodies can reseed an existing clip when a native transition releases.
    // Keep its normalized progress fresh through the transition and a short release tail.
    // The server follows the sender's cadence instead of imposing a second jitter-sensitive gate.
    const bool refresh_active = now < active_progress_until_ &&
                                (!limit_motion || elapsed + 1e-8 >= kBodyMotionPublicationSeconds);
    // Starts/stops and authority boundaries never wait for a movement or idle timer. Idle
    // refreshes carry current native progress, recover a lost last report and support reseeding.
    if (!critical && !retry_critical && !refresh_active && elapsed >= 0 &&
        elapsed + 1e-8 < kBodyIdlePublicationSeconds &&
        ((limit_motion && elapsed + 1e-8 < kBodyMotionPublicationSeconds) || same_motion(state))) {
      return false;
    }
    if (critical) {
      critical_at_ = now;
    }
    previous_ = state;
    animations_ = *animations;
    published_at_ = now;
    return true;
  }

 private:
  bool same_starts(const local::AnimationState& current) const {
    for (unsigned layer = 0; layer < current.size(); ++layer) {
      const auto& a = animations_[layer];
      const auto& b = current[layer];
      if (a.sequence != b.sequence || a.clip != b.clip) {
        return false;
      }
      if (!b.sequence) {
        continue;
      }
      // The clock of the same native start advances without a new start sequence. Replicas
      // already play it locally; changing it alone must not resend every animation layer.
      if (a.params.transition != b.params.transition || a.params.speed != b.params.speed ||
          a.params.weight != b.params.weight || a.params.layer != b.params.layer ||
          a.params.token != b.params.token || a.params.flags != b.params.flags ||
          a.params.user_token != b.params.user_token) {
        return false;
      }
      for (unsigned i = 0; i < 8; ++i) {
        if (a.params.user_data[i] != b.params.user_data[i]) {
          return false;
        }
      }
    }
    return true;
  }

  static bool same_vec(core::Vec3 a, core::Vec3 b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
  }

  static bool same_quat(core::Quat a, core::Quat b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
  }

  bool same_motion(const BodyState& state) const {
    return previous_ && same_vec(previous_->position, state.position) &&
           same_vec(previous_->velocity, state.velocity) &&
           same_quat(previous_->rotation, state.rotation) &&
           same_quat(previous_->motion_rotation, state.motion_rotation) &&
           previous_->params == state.params;
  }

  std::optional<BodyState> previous_;
  local::AnimationState animations_{};
  double published_at_ = 0;
  double critical_at_ = 0;
  double active_progress_until_ = 0;
};

}  // namespace kcd2s::integration
