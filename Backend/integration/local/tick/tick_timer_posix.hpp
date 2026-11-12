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
#ifdef __APPLE__
    return "nanosleep (macOS)";
#else
    return "clock_nanosleep on the monotonic clock";
#endif
  }

  // Blocks for about `seconds` (no-op when not positive). The caller re-checks its clock, so an
  // early or late wake-up only changes this tick's lateness, never the schedule.
  void wait(double seconds) {
    if (!(seconds > 0.0)) {
      return;
    }
#ifdef __APPLE__
    // macOS has no clock_nanosleep: a relative sleep, continued with what is left when a signal
    // cuts it short. The kernel wakes it within about a tenth of a millisecond.
    timespec left{};
    left.tv_sec = static_cast<time_t>(seconds);
    left.tv_nsec = static_cast<long>((seconds - double(left.tv_sec)) * 1.0e9);
    while (nanosleep(&left, &left) == -1 && errno == EINTR) {
    }
#else
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const long long due_ns = static_cast<long long>(now.tv_sec) * 1000000000LL + now.tv_nsec +
                             static_cast<long long>(seconds * 1.0e9) + 1;
    timespec due{};
    due.tv_sec = static_cast<time_t>(due_ns / 1000000000LL);
    due.tv_nsec = static_cast<long>(due_ns % 1000000000LL);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &due, nullptr) == EINTR) {
    }
#endif
  }
};

}  // namespace kcd2s::local::tick
