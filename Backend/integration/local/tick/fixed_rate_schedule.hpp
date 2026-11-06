#pragma once
// Fixed-rate tick schedule of the local server loop. Portable bookkeeping only; the
// Windows wait lives in tick_timer_win.hpp.
//
// Ticks are anchored to absolute deadlines start + n * interval, so timer wake-up jitter does not
// accumulate into a slower rate. A tick that starts late keeps the cadence (the next deadline is
// still one interval after this tick's deadline). When the loop falls a whole interval or more
// behind, the missed slots are skipped and counted instead of being run back to back, so an
// overloaded server reports the shortfall rather than bursting to catch up.
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>

namespace kcd2s::local::tick {

inline constexpr double kDefaultTickRateHz = 60.0;
inline constexpr double kMinimumTickRateHz = 10.0;
inline constexpr double kMaximumTickRateHz = 240.0;

// Parses the KCDS_SERVER_TICK_HZ value: a tick rate in [10, 240] Hz. Anything else, including an
// unset or empty variable, means "use the default".
inline std::optional<double> parse_tick_rate(const char* text) {
  if (!text || !*text) {
    return std::nullopt;
  }
  char* end = nullptr;
  const double hz = std::strtod(text, &end);
  if (end == text || *end != '\0' || !(hz >= kMinimumTickRateHz) || hz > kMaximumTickRateHz) {
    return std::nullopt;
  }
  return hz;
}

// What the schedule decided for one tick.
struct TickSlot {
  double deadline = 0.0;      // scheduled start of this tick (seconds, same clock as `now`)
  double lateness = 0.0;      // how long after its deadline the tick started (>= 0)
  std::uint64_t skipped = 0;  // whole slots dropped just before this tick
};

class FixedRateSchedule {
 public:
  explicit FixedRateSchedule(double rate_hz) : interval_(1.0 / rate_hz) {
  }

  double interval() const {
    return interval_;
  }

  // The first tick is due immediately.
  void start(double now) {
    next_deadline_ = now;
  }

  // Start time of the next tick. The loop waits until this time before calling begin_tick().
  double next_deadline() const {
    return next_deadline_;
  }

  // Claims the slot for a tick that starts at `now` and advances the schedule.
  TickSlot begin_tick(double now) {
    TickSlot slot;
    slot.deadline = next_deadline_;
    double late = now - next_deadline_;
    if (late < 0.0) {
      late = 0.0;  // started marginally early (timer resolution); still this slot
    }
    if (late >= interval_) {
      const double whole_slots = std::floor(late / interval_);
      slot.skipped = static_cast<std::uint64_t>(whole_slots);
      slot.deadline = next_deadline_ + whole_slots * interval_;
      late = now - slot.deadline;
      if (late < 0.0) {
        late = 0.0;
      }
    }
    slot.lateness = late;
    next_deadline_ = slot.deadline + interval_;
    ticks_ += 1;
    skipped_total_ += slot.skipped;
    return slot;
  }

  std::uint64_t ticks() const {
    return ticks_;
  }
  std::uint64_t skipped_total() const {
    return skipped_total_;
  }

 private:
  double interval_;
  double next_deadline_ = 0.0;
  std::uint64_t ticks_ = 0;
  std::uint64_t skipped_total_ = 0;
};

}  // namespace kcd2s::local::tick
