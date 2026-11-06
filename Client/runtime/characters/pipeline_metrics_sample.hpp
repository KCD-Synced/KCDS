#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace kcds::runtime::characters::pipeline_metrics {

// One thread and one pipeline phase per window. These windows are independent;
// asynchronous Present work must never be added to main-thread post-phase wall time.
struct TimingWindow {
  std::int64_t started = 0;
  std::int64_t ended = 0;
  std::uint64_t samples = 0;
  std::uint64_t invalid = 0;
  std::uint64_t aborted = 0;
  std::uint64_t nested = 0;
  std::uint64_t test_calls = 0;
  std::uint64_t overlay_attempts = 0;
  std::uint64_t flag_skip_calls = 0;
  double first_sum_ms = 0;
  double first_max_ms = 0;
  double second_sum_ms = 0;
  double second_max_ms = 0;
  double total_sum_ms = 0;
  double total_max_ms = 0;

  bool add(std::int64_t begin, std::int64_t split, std::int64_t end, std::int64_t frequency,
           bool overlay_attempted = false) {
    if (begin <= 0 || split < begin || end < split || frequency <= 0 || (ended && begin < ended)) {
      ++invalid;
      return false;
    }
    if (!started) {
      started = begin;
    }
    ended = end;
    const double milliseconds_per_tick = 1000.0 / double(frequency);
    const double first_ms = double(split - begin) * milliseconds_per_tick;
    const double second_ms = double(end - split) * milliseconds_per_tick;
    const double total_ms = double(end - begin) * milliseconds_per_tick;
    first_sum_ms += first_ms;
    second_sum_ms += second_ms;
    total_sum_ms += total_ms;
    first_max_ms = std::max(first_max_ms, first_ms);
    second_max_ms = std::max(second_max_ms, second_ms);
    total_max_ms = std::max(total_max_ms, total_ms);
    overlay_attempts += overlay_attempted ? 1 : 0;
    ++samples;
    return true;
  }

  double elapsed_ms(std::int64_t frequency) const {
    return started > 0 && ended >= started && frequency > 0
               ? double(ended - started) * (1000.0 / double(frequency))
               : 0.0;
  }
};

enum class PostChild : std::size_t { render, render_end, animation_finish, count };
enum class AnimationSpan : std::size_t {
  pose_finish,
  attachment_update,
  attachment_refresh,
  count
};

// These class-method spans can contain one another. Each kind excludes its own
// recursive calls, but different kinds are inclusive and must never be summed.
struct AnimationDetailSample {
  std::int64_t begin = 0;
  std::array<std::int64_t, 3> ticks{};
  std::array<std::int64_t, 3> last_end{};
  std::array<std::uint64_t, 3> calls{};
  std::uint64_t nested = 0;
  bool valid = true;

  void add(AnimationSpan kind, std::int64_t start, std::int64_t end) {
    const auto index = static_cast<std::size_t>(kind);
    if (index >= ticks.size() || begin <= 0 || start < begin || end < start ||
        (last_end[index] && start < last_end[index])) {
      valid = false;
      return;
    }
    ticks[index] += end - start;
    last_end[index] = end;
    ++calls[index];
  }

  bool complete(std::int64_t end) const {
    if (!valid || begin <= 0 || end < begin) {
      return false;
    }
    for (const auto last : last_end) {
      if (last > end) {
        return false;
      }
    }
    return true;
  }
};

struct ThreadCpuStamp {
  std::uint64_t kernel = 0;
  std::uint64_t user = 0;
  bool valid = false;
};

// Only outermost child calls on the post thread enter this partition. Nested
// calls are already covered by their parent's elapsed time and are counted only.
struct PostDetailSample {
  std::int64_t begin = 0;
  std::int64_t last_child_end = 0;
  std::array<std::int64_t, 3> child_ticks{};
  std::array<std::uint64_t, 3> child_calls{};
  AnimationDetailSample animation;
  std::uint64_t nested_children = 0;
  bool valid = true;

  void add_child(PostChild child, std::int64_t start, std::int64_t end) {
    const auto index = static_cast<std::size_t>(child);
    if (index >= child_ticks.size() || begin <= 0 || start < begin || end < start ||
        (last_child_end && start < last_child_end)) {
      valid = false;
      return;
    }
    child_ticks[index] += end - start;
    ++child_calls[index];
    last_child_end = end;
  }
};

struct PostDetailWindow {
  std::uint64_t partition_samples = 0;
  std::uint64_t partition_invalid = 0;
  std::uint64_t nested_children = 0;
  std::array<std::uint64_t, 3> child_calls{};
  std::array<double, 3> child_sum_ms{};
  std::array<double, 3> child_max_ms{};
  std::uint64_t animation_samples = 0;
  std::uint64_t animation_invalid = 0;
  std::uint64_t animation_nested = 0;
  std::array<std::uint64_t, 3> animation_calls{};
  std::array<double, 3> animation_sum_ms{};
  std::array<double, 3> animation_max_ms{};
  double other_sum_ms = 0;
  double other_max_ms = 0;
  std::uint64_t cpu_samples = 0;
  std::uint64_t cpu_invalid = 0;
  double cpu_sum_ms = 0;
  double cpu_max_ms = 0;

  // Called only after the outer wall-time sample is accepted. An invalid child
  // partition or CPU read must not dilute the other independent denominator.
  void add(const PostDetailSample& sample, std::int64_t end, std::int64_t frequency,
           const ThreadCpuStamp& cpu_begin, const ThreadCpuStamp& cpu_end) {
    nested_children += sample.nested_children;
    if (!sample.valid || sample.begin <= 0 || end < sample.begin || frequency <= 0 ||
        sample.last_child_end > end) {
      ++partition_invalid;
    } else {
      const double scale = 1000.0 / double(frequency);
      std::int64_t remaining = end - sample.begin;
      for (std::size_t index = 0; index < child_calls.size(); ++index) {
        remaining -= sample.child_ticks[index];
        const double milliseconds = double(sample.child_ticks[index]) * scale;
        child_calls[index] += sample.child_calls[index];
        child_sum_ms[index] += milliseconds;
        child_max_ms[index] = std::max(child_max_ms[index], milliseconds);
      }
      const double other_ms = double(remaining) * scale;
      other_sum_ms += other_ms;
      other_max_ms = std::max(other_max_ms, other_ms);
      ++partition_samples;
      animation_nested += sample.animation.nested;
      // No manager call is a valid empty detail sample. A recovered child fault
      // invalidates these inclusive spans without losing the outer partition.
      if (sample.animation.begin == 0 || sample.animation.complete(end)) {
        ++animation_samples;
        for (std::size_t index = 0; index < animation_calls.size(); ++index) {
          const double milliseconds = double(sample.animation.ticks[index]) * scale;
          animation_calls[index] += sample.animation.calls[index];
          animation_sum_ms[index] += milliseconds;
          animation_max_ms[index] = std::max(animation_max_ms[index], milliseconds);
        }
      } else {
        ++animation_invalid;
      }
    }
    if (!cpu_begin.valid || !cpu_end.valid || cpu_end.kernel < cpu_begin.kernel ||
        cpu_end.user < cpu_begin.user) {
      ++cpu_invalid;
    } else {
      // FILETIME units are 100 ns: 10,000 units per millisecond. Subtract before
      // converting, and sum as doubles to avoid overflowing cumulative counters.
      const double milliseconds =
          (double(cpu_end.kernel - cpu_begin.kernel) + double(cpu_end.user - cpu_begin.user)) /
          10000.0;
      cpu_sum_ms += milliseconds;
      cpu_max_ms = std::max(cpu_max_ms, milliseconds);
      ++cpu_samples;
    }
  }
};

}  // namespace kcds::runtime::characters::pipeline_metrics
