#pragma once
// Remote players shown smoothly. A replica used to take its owner's newest snapshot
// position every frame: a snapshot that came late (a server host that woke its tick late, internet
// jitter) showed as a stop and a jump, and above 60 fps the replica moved in steps.
//
// The owner sends its own clock with its movement (the NetworkPlayer field "motionClock", in ms).
// A replica keeps a short timeline of what its owner sent and shows the owner as it was
// kDelaySeconds ago on the owner's clock: the position and rotation between the two samples
// around that moment, and the rest of the owner's state (animations, jump, ...) from the earlier
// one, so everything stays in step. The owner's clock is matched to the local clock by the
// fastest delivery of the last kOffsetWindowSeconds, so late deliveries do not move the timeline;
// the match follows changes at most kOffsetSlew as fast as time passes, so the replica only speeds
// up or slows down a little and never steps back.
// When the samples run out the replica keeps the owner's last velocity for at most
// kExtrapolateSeconds and then stands. A sample farther than the teleport distance from the last
// one (a teleport, a respawn), and faster than anything walks or rides, restarts the timeline
// there, so a teleport is not shown as a slide.
//
// In a big crowd the server's byte budget spreads each body's updates apart (800 moving
// players, about 1.5 a second each). The delay then grows with the time between its samples, so the
// body keeps moving between them instead of walking, standing and jumping. It changes slower than
// time passes, so a body only slows down a little and never steps back.
//
// A link whose deliveries vary is the same: the delay grows to cover how much later than the
// fastest its samples come, and stays at its least on a steady link, so a good connection is not
// kept waiting for a bad one's margin.
//
// A sample can still come later than the delay covers. The body has then gone on with its velocity
// (for as long as its samples are usually apart) and stood, and the timeline is suddenly elsewhere:
// shown as it is, that was a jump of half a metre in one frame.
// What a frame's step has beyond the body's pace is kept as a correction that fades out in a
// moment, so the body catches up instead of jumping. A teleport restarts the timeline and is still
// shown at once.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <utility>

#include "kcd2s/core/math.hpp"

namespace kcd2s::integration {

struct TimelinePose {
  core::Vec3 position;
  core::Quat rotation;
};

inline TimelinePose interpolate_pose(const TimelinePose& from, const TimelinePose& to, float t) {
  TimelinePose result;
  result.position.x = from.position.x + (to.position.x - from.position.x) * t;
  result.position.y = from.position.y + (to.position.y - from.position.y) * t;
  result.position.z = from.position.z + (to.position.z - from.position.z) * t;
  // Normalized linear interpolation along the shorter arc.
  const core::Quat& a = from.rotation;
  core::Quat b = to.rotation;
  if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f) {
    b = core::Quat{-b.x, -b.y, -b.z, -b.w};
  }
  core::Quat q{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
               a.w + (b.w - a.w) * t};
  const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (length > 1e-6f) {
    q = core::Quat{q.x / length, q.y / length, q.z / length, q.w / length};
  } else {
    q = a;
  }
  result.rotation = q;
  return result;
}

template <typename State>
class ReplicaTimeline {
 public:
  // How far behind its owner's clock a replica is shown at least.
  static constexpr double kDelaySeconds = 0.05;
  // And at most, however rarely its samples come.
  static constexpr double kMaxDelaySeconds = 1.2;
  // The delay in times the gap between samples, how fast it changes in seconds per second, and the
  // share of a shorter gap the measure takes (a longer one counts at once).
  static constexpr double kDelayGaps = 1.3;
  static constexpr double kDelaySlew = 0.12;
  static constexpr double kGapFall = 0.02;
  // A shorter delay is taken only once it is this much below the delay, and more slowly; the rate
  // of change itself eases in and out at kDelayEase (seconds per second, per second). A delay that
  // chased each small change of its measure moved the body 12 % faster or slower for a frame or
  // two, several times a second: a crowd walked unevenly.
  static constexpr double kDelayHold = 0.06;
  static constexpr double kDelayFallSlew = 0.05;
  static constexpr double kDelayEase = 0.4;
  // The delay in times how much later than the fastest delivery the samples come.
  static constexpr double kDelayLateness = 1.1;
  // A jump this fast over the owner's clock is a teleport, not a late sample (metres a second).
  static constexpr double kFastestMetersPerSecond = 20.0;
  // How long the owner's last velocity is kept when the samples run out, and the span it is
  // measured over.
  static constexpr double kExtrapolateSeconds = 0.1;
  static constexpr double kVelocitySeconds = 0.05;
  // Longer when its samples are that far apart, up to this.
  static constexpr double kMostExtrapolateSeconds = 0.4;
  // A frame's step counts as the body's own up to its pace times this, plus this many metres a
  // second.
  static constexpr double kPaceMargin = 1.25;
  static constexpr double kPaceSlack = 1.0;
  // How fast a correction fades (seconds to about a third of it), and at most, so a large one is
  // not a jump of its own (metres a second).
  static constexpr double kCorrectionSeconds = 0.15;
  static constexpr double kFastestCatchUp = 5.0;
  // How much of a new pair of samples the running pace takes, by the seconds between them: one hop
  // between two samples close together does not make the body fast.
  static constexpr double kPaceSeconds = 0.5;
  static constexpr double kFastestOnFoot = 8.0;
  // How long a delivery counts for matching the owner's clock to the local one.
  static constexpr double kOffsetWindowSeconds = 2.0;
  // How fast the clock match follows its target, in seconds per second, after the samples of the
  // first kQuickMatchSeconds, which set it directly. Faster deliveries are followed only once the
  // match is kOffsetHold behind them, and more slowly: they only mean the body could be shown a
  // little sooner. It moves every frame, eased like the delay; moved by a whole step when a sample
  // came, a body in a crowd (a sample every third of a second) jumped by a tenth of that.
  static constexpr double kOffsetSlew = 0.1;
  static constexpr double kOffsetFallSlew = 0.05;
  static constexpr double kOffsetHold = 0.02;
  static constexpr double kQuickMatchSeconds = 0.5;
  // Samples kept at most (a second of 60 Hz movement).
  static constexpr std::size_t kMaxSamples = 64;

  explicit ReplicaTimeline(float teleport_meters) : teleport_meters_(teleport_meters) {
  }

  // The owner's state as it sent it at owner_ms on its clock, with its pose, received at local
  // time `now` (seconds). A sample that came late takes its place on the timeline; one the
  // timeline already has is ignored (false).
  bool add(std::int64_t owner_ms, const State& state, const TimelinePose& pose, double now,
           bool stationary = false) {
    const double owner = double(owner_ms) / 1000.0;
    if (!samples_.empty() && owner < samples_.back().owner - kOffsetWindowSeconds) {
      clear();  // the owner's clock started over (a new session)
    }
    auto place = std::upper_bound(samples_.begin(), samples_.end(), owner,
                                  [](double at, const Sample& s) { return at < s.owner; });
    if (place != samples_.begin() && std::prev(place)->owner == owner) {
      return false;
    }
    bool moving_sample = false;
    if (place == samples_.end() && !samples_.empty()) {
      const float jump = core::distance(samples_.back().pose.position, pose.position);
      const double since = owner - samples_.back().owner;
      if (jump > teleport_meters_ && jump > since * kFastestMetersPerSecond) {
        samples_.clear();
        place = samples_.end();
        forget_correction();
      } else if (!stationary && !samples_.back().stationary) {
        note_gap(since);
        note_pace(jump, since);
        moving_sample = true;
      }
    }
    samples_.insert(place, Sample{owner, state, pose, stationary});
    while (samples_.size() > kMaxSamples) {
      samples_.pop_front();
    }
    offsets_.emplace_back(now, now - owner);
    while (!offsets_.empty() && offsets_.front().first < now - kOffsetWindowSeconds) {
      offsets_.pop_front();
    }
    double fastest = offsets_.front().second;
    for (const auto& entry : offsets_) {
      fastest = std::min(fastest, entry.second);
    }
    if (moving_sample) {
      note_lateness(now - owner - fastest);
    }
    // A replica that just appeared takes the fastest delivery at once; later the match follows it
    // slowly, frame by frame (offset_at), so the replica never visibly jumps in time.
    if (!has_offset_) {
      has_offset_ = true;
      first_matched_ = now;
    }
    offset_target_ = fastest;
    if (now - first_matched_ < kQuickMatchSeconds) {
      offset_ = fastest;
      offset_rate_ = 0.0;
      offset_updated_ = now;
    }
    return true;
  }

  const State* newest() const {
    return samples_.empty() ? nullptr : &samples_.back().state;
  }

  // A server field or epoch correction can arrive without a new owner clock. Updating that
  // payload must not create an artificial arrival/clock-offset sample. An untimed pose correction
  // has no interpolation span, so discard older poses rather than slide through the correction.
  bool refresh_newest(std::int64_t owner_ms, const State& state, const TimelinePose& pose,
                      bool stationary) {
    if (samples_.empty() || samples_.back().owner != double(owner_ms) / 1000.0) {
      return false;
    }
    if (!(samples_.back().pose.position == pose.position) ||
        !(samples_.back().pose.rotation == pose.rotation)) {
      samples_.clear();
      samples_.push_back(Sample{double(owner_ms) / 1000.0, state, pose, stationary});
      forget_correction();
    } else {
      samples_.back().state = state;
      samples_.back().stationary = stationary;
    }
    return true;
  }

  // The state to show at local time `now` and the pose it has then; nullptr before any sample.
  const State* shown(double now, TimelinePose& pose) const {
    const State* state = on_timeline(now, pose);
    if (state) {
      smooth(now, pose);
    }
    return state;
  }

  void clear() {
    samples_.clear();
    offsets_.clear();
    has_offset_ = false;
    first_matched_ = 0.0;
    offset_target_ = 0.0;
    offset_rate_ = 0.0;
    offset_falling_ = false;
    gap_ = 0.0;
    lateness_ = 0.0;
    delay_ = kDelaySeconds;
    delay_updated_ = -1.0;
    delay_rate_ = 0.0;
    delay_falling_ = false;
    forget_correction();
  }

  // How far behind its owner's clock the replica is shown now.
  double delay() const {
    return delay_;
  }

  std::size_t sample_count() const {
    return samples_.size();
  }

 private:
  // The pose on the timeline at local time `now`, before the correction.
  const State* on_timeline(double now, TimelinePose& pose) const {
    pace_ = 0.0;
    if (samples_.empty()) {
      return nullptr;
    }
    const double at = now - offset_at(now) - delay_at(now);
    if (at <= samples_.front().owner) {
      pose = samples_.front().pose;
      return &samples_.front().state;
    }
    for (std::size_t i = 1; i < samples_.size(); ++i) {
      const Sample& before = samples_[i - 1];
      const Sample& after = samples_[i];
      if (at < after.owner) {
        const double span = after.owner - before.owner;
        if (before.stationary && span > kExtrapolateSeconds) {
          // Idle heartbeats may be seconds apart. A resumed walk/turn/action starts at the new
          // sample, not gradually across the silent interval preceding it.
          pose = before.pose;
          return &before.state;
        }
        const float t = span > 0.0 ? float((at - before.owner) / span) : 1.0f;
        pose = interpolate_pose(before.pose, after.pose, t);
        if (span > 0.0) {
          pace_ = core::distance(before.pose.position, after.pose.position) / span;
        }
        return &before.state;
      }
    }
    // Past the newest sample: its velocity for a short while, then it stands. The velocity is
    // taken over at least kVelocitySeconds: the owner's clock has whole milliseconds and its frames
    // are uneven, so two neighbouring samples alone give a noisy speed.
    const Sample& last = samples_.back();
    pose = last.pose;
    if (!last.stationary && samples_.size() >= 2) {
      std::size_t first = samples_.size() - 2;
      while (first > 0 && last.owner - samples_[first].owner < kVelocitySeconds &&
             !samples_[first - 1].stationary) {
        --first;
      }
      const Sample& previous = samples_[first];
      const double span = last.owner - previous.owner;
      if (span > 0.0 && !(previous.stationary && span > kExtrapolateSeconds)) {
        const double most = std::clamp(gap_, kExtrapolateSeconds, kMostExtrapolateSeconds);
        const float ahead = float(std::min(at - last.owner, most) / span);
        pace_ = core::distance(previous.pose.position, last.pose.position) / span;
        pose.position.x += (last.pose.position.x - previous.pose.position.x) * ahead;
        pose.position.y += (last.pose.position.y - previous.pose.position.y) * ahead;
        pose.position.z += (last.pose.position.z - previous.pose.position.z) * ahead;
      }
    }
    return &last.state;
  }

  // Keeps what a frame's step has beyond the body's pace as a fading correction of the pose.
  void smooth(double now, TimelinePose& pose) const {
    const core::Vec3 target = pose.position;
    if (smooth_started_) {
      const double passed = std::max(0.0, now - smooth_at_);
      const float held = std::sqrt(correction_.x * correction_.x + correction_.y * correction_.y +
                                   correction_.z * correction_.z);
      const float faded = held * float(1.0 - std::exp(-passed / kCorrectionSeconds));
      const float most = float(passed * kFastestCatchUp);
      const float fade = held > 0.0f ? 1.0f - (std::min)(faded, most) / held : 0.0f;
      correction_.x *= fade;
      correction_.y *= fade;
      correction_.z *= fade;
      const float step = core::distance(smooth_target_, target);
      // A person's own speed always counts, so one who starts running does not lag; beyond it only
      // what the body has kept up (a rider).
      const double pace = std::min(pace_, std::max(running_pace_ * kPaceMargin, kFastestOnFoot));
      const float own = float(passed * (pace * kPaceMargin + kPaceSlack));
      if (step > own && step > 0.0f) {
        const float beyond = 1.0f - own / step;
        correction_.x += (smooth_target_.x - target.x) * beyond;
        correction_.y += (smooth_target_.y - target.y) * beyond;
        correction_.z += (smooth_target_.z - target.z) * beyond;
      }
      const float size = std::sqrt(correction_.x * correction_.x + correction_.y * correction_.y +
                                   correction_.z * correction_.z);
      if (size > teleport_meters_) {
        correction_ = core::Vec3{};
      }
    }
    smooth_started_ = true;
    smooth_at_ = now;
    smooth_target_ = target;
    pose.position.x += correction_.x;
    pose.position.y += correction_.y;
    pose.position.z += correction_.z;
  }

  void forget_correction() {
    smooth_started_ = false;
    correction_ = core::Vec3{};
  }

  void note_pace(float jump, double since) {
    if (since <= 0.0) {
      return;
    }
    const double share = std::min(since / kPaceSeconds, 1.0);
    running_pace_ += (double(jump) / since - running_pace_) * share;
  }

  void note_gap(double since) {
    // Longer than the delay can cover: a pause, not how often the samples come.
    if (since > kMaxDelaySeconds) {
      return;
    }
    gap_ = since > gap_ ? since : gap_ + (since - gap_) * kGapFall;
  }

  void note_lateness(double late) {
    // Longer than the delay can cover: a pause, not how the link varies.
    if (late > kMaxDelaySeconds) {
      return;
    }
    lateness_ = late > lateness_ ? late : lateness_ + (late - lateness_) * kGapFall;
  }

  // A share of how far behind the owner the body is shown (the delay, or the clock match), moved
  // toward its target: up (showing the owner as it was longer ago) as soon as it falls short, down
  // only once it is `hold` above the target and then all the way, more slowly. Its rate eases in
  // and out at kDelayEase, so the body's pace changes gradually.
  static void follow(double& value, double& rate, bool& falling, double target, double passed,
                     double rise, double fall, double hold) {
    if (target > value) {
      falling = false;
    } else if (value - target > hold) {
      falling = true;
    }
    double wanted_rate = 0.0;
    if (target > value) {
      wanted_rate = rise;
    } else if (falling && target < value) {
      wanted_rate = -fall;
    }
    // Slow down in time to stop at the target instead of stopping at once.
    const double stopping = std::sqrt(2.0 * kDelayEase * std::fabs(target - value));
    wanted_rate = std::clamp(wanted_rate, -stopping, stopping);
    // A target that moves toward the value can still be passed a little; the rate then eases back
    // instead of stopping at once, which would itself be a jerk.
    const double ease = kDelayEase * passed;
    rate += std::clamp(wanted_rate - rate, -ease, ease);
    value += rate * passed;
    if (value <= target) {
      falling = false;
    }
  }

  double delay_at(double now) const {
    if (delay_updated_ >= 0.0) {
      const double wanted = std::max(gap_ * kDelayGaps, lateness_ * kDelayLateness);
      const double target = std::clamp(wanted, kDelaySeconds, kMaxDelaySeconds);
      follow(delay_, delay_rate_, delay_falling_, target, std::max(0.0, now - delay_updated_),
             kDelaySlew, kDelayFallSlew, kDelayHold);
      delay_ = std::clamp(delay_, kDelaySeconds, kMaxDelaySeconds);
    }
    delay_updated_ = now;
    return delay_;
  }

  double offset_at(double now) const {
    follow(offset_, offset_rate_, offset_falling_, offset_target_,
           std::max(0.0, now - offset_updated_), kOffsetSlew, kOffsetFallSlew, kOffsetHold);
    offset_updated_ = now;
    return offset_;
  }

  struct Sample {
    double owner = 0.0;  // the owner's clock, seconds
    State state;
    TimelinePose pose;
    bool stationary = false;
  };
  std::deque<Sample> samples_;
  // Local arrival time and (arrival - owner clock) of the recent samples, and the clock match.
  std::deque<std::pair<double, double>> offsets_;
  mutable double offset_ = 0.0;
  bool has_offset_ = false;
  double first_matched_ = 0.0;
  double offset_target_ = 0.0;
  mutable double offset_updated_ = 0.0;
  mutable double offset_rate_ = 0.0;
  mutable bool offset_falling_ = false;
  // The time between its moving samples on the owner's clock, and the delay following it.
  double gap_ = 0.0;
  double lateness_ = 0.0;
  mutable double delay_ = kDelaySeconds;
  mutable double delay_updated_ = -1.0;
  mutable double delay_rate_ = 0.0;
  mutable bool delay_falling_ = false;
  // What the frames' steps had beyond the body's pace, and the frame it was measured at.
  mutable double pace_ = 0.0;
  double running_pace_ = 0.0;
  mutable bool smooth_started_ = false;
  mutable double smooth_at_ = 0.0;
  mutable core::Vec3 smooth_target_{};
  mutable core::Vec3 correction_{};
  float teleport_meters_;
};

}  // namespace kcd2s::integration
