#pragma once
// Wait for the fixed-rate server tick on Linux and other POSIX systems: the same
// interface as tick_timer_win.hpp, on the monotonic clock with nanosleep precision.
#ifdef _WIN32
#error "tick_timer_posix.hpp is for POSIX systems; Windows uses tick_timer_win.hpp"
#endif

#include <cerrno>
#include <ctime>

namespace kcd2s::local::tick {

class TickTimer {
 public:
  TickTimer() = default;
  TickTimer(const TickTimer&) = delete;
  TickTimer& operator=(const TickTimer&) = delete;

  bool high_resolution() const {
    return true;
  }
  const char* description() const {
    return "clock_nanosleep on the monotonic clock";
  }

  // Blocks for about `seconds` (no-op when not positive). The caller re-checks its clock, so an
  // early or late wake-up only changes this tick's lateness, never the schedule.
  void wait(double seconds) {
    if (!(seconds > 0.0)) {
      return;
    }
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const long long due_ns = static_cast<long long>(now.tv_sec) * 1000000000LL + now.tv_nsec +
                             static_cast<long long>(seconds * 1.0e9) + 1;
    timespec due{};
    due.tv_sec = static_cast<time_t>(due_ns / 1000000000LL);
    due.tv_nsec = static_cast<long>(due_ns % 1000000000LL);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &due, nullptr) == EINTR) {
    }
  }
};

}  // namespace kcd2s::local::tick
