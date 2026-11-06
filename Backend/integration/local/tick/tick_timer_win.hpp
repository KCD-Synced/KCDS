#pragma once
// Windows wait for the fixed-rate server tick. Uses a high-resolution waitable timer
// (Windows 10 1803 and later), which wakes within about a millisecond of the due time without
// changing the system-wide timer resolution (no timeBeginPeriod). Older systems fall back to a
// standard waitable timer, which is limited by the system timer resolution; the server reports
// which one is in use.
#ifndef _WIN32
#error "tick_timer_win.hpp is Windows-only"
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <stdexcept>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace kcd2s::local::tick {

class TickTimer {
 public:
  TickTimer() {
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                    TIMER_ALL_ACCESS);
    high_resolution_ = timer_ != nullptr;
    if (!timer_) {
      timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    if (!timer_) {
      throw std::runtime_error("CreateWaitableTimerExW failed");
    }
  }
  ~TickTimer() {
    if (timer_) {
      CloseHandle(timer_);
    }
  }
  TickTimer(const TickTimer&) = delete;
  TickTimer& operator=(const TickTimer&) = delete;

  bool high_resolution() const {
    return high_resolution_;
  }

  // Blocks for about `seconds` (no-op when not positive). The caller re-checks its clock, so an
  // early or late wake-up only changes this tick's lateness, never the schedule.
  void wait(double seconds) {
    if (!(seconds > 0.0)) {
      return;
    }
    LARGE_INTEGER due;
    // Negative due time = relative interval in 100 ns units; round up so the wait is never zero.
    due.QuadPart = -static_cast<LONGLONG>(seconds * 1.0e7 + 1.0);
    if (!SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
      throw std::runtime_error("SetWaitableTimer failed");
    }
    WaitForSingleObject(timer_, INFINITE);
  }

 private:
  HANDLE timer_ = nullptr;
  bool high_resolution_ = false;
};

}  // namespace kcd2s::local::tick
