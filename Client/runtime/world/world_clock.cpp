#include "world_clock.hpp"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace kcds::runtime::world {
namespace {

using SetTime = void (*)(void* calendar, std::int64_t milliseconds);
using SetRatio = void (*)(void* calendar, float ratio);

constexpr std::size_t kGlobalsCalendar = 0x88;  // the global object's C_Calendar*
constexpr std::size_t kCalendarTime = 72;       // C_Calendar: game milliseconds, 64 bit

constexpr std::int64_t kDayMilliseconds = 86400000;
constexpr std::int64_t kHourMilliseconds = 3600000;
// A clock this close to the server's is not touched.
constexpr std::int64_t kToleranceMilliseconds = 1000;

Natives natives;
Logger report = nullptr;
// The ratio last set, so an unchanged one is not set again (the call also touches the sky).
float last_ratio = -1.0f;
// A clock moved while it was to stand still: the sky takes its new hour only on its own next
// update, which a stopped clock never runs (set to 23 with speed 0, the day sky stayed, live). So
// it runs at kSettleRatio for kSettleMilliseconds first and then stops (frame()).
constexpr float kSettleRatio = 1.0f;
constexpr unsigned long long kSettleMilliseconds = 250;
unsigned long long settle_until = 0;

void* calendar() {
  if (!natives.game_globals || !*natives.game_globals) {
    return nullptr;
  }
  return *reinterpret_cast<void**>(static_cast<std::uint8_t*>(*natives.game_globals) +
                                   kGlobalsCalendar);
}

}  // namespace

bool install(const Natives& found, Logger logger) {
  natives = found;
  report = logger;
  return natives.game_globals && natives.set_time && natives.set_ratio;
}

long long current_day() {
  const auto* clock = static_cast<const std::uint8_t*>(calendar());
  if (!clock) {
    return -1;
  }
  const auto* stored = reinterpret_cast<const std::int64_t*>(clock + kCalendarTime);
  return static_cast<long long>(*stored / kDayMilliseconds);
}

bool apply(const kcd2s::integration::WorldTime& time) {
  auto* clock = static_cast<std::uint8_t*>(calendar());
  if (!clock || !natives.set_time || !natives.set_ratio) {
    return false;
  }
  const std::int64_t target = std::int64_t(time.day) * kDayMilliseconds +
                              std::int64_t(double(time.hour) * kHourMilliseconds);
  auto* stored = reinterpret_cast<std::int64_t*>(clock + kCalendarTime);
  const std::int64_t before = *stored;
  const std::int64_t drift = before > target ? before - target : target - before;
  if (drift > kToleranceMilliseconds) {
    if (before > target) {
      // set_time moves forward only: start just before the target.
      *stored = target > 0 ? target - 1 : 0;
    }
    reinterpret_cast<SetTime>(natives.set_time)(clock, target);
    if (report) {
      report("WORLD CLOCK set day=%u hour=%.3f (was %lld ms)", time.day, double(time.hour),
             static_cast<long long>(before));
    }
    if (time.timeScale == 0.0f) {
      // Running a moment, so the sky takes the new hour; frame() stops it.
      reinterpret_cast<SetRatio>(natives.set_ratio)(clock, kSettleRatio);
      last_ratio = 0.0f;
      settle_until = GetTickCount64() + kSettleMilliseconds;
      return true;
    }
  }
  if (time.timeScale != last_ratio) {
    last_ratio = time.timeScale;
    settle_until = 0;
    reinterpret_cast<SetRatio>(natives.set_ratio)(clock, time.timeScale);
    if (report) {
      report("WORLD CLOCK speed %.3f", double(time.timeScale));
    }
  }
  return true;
}

void frame() {
  if (settle_until == 0 || GetTickCount64() < settle_until) {
    return;
  }
  settle_until = 0;
  auto* clock = calendar();
  if (!clock || !natives.set_ratio) {
    return;
  }
  reinterpret_cast<SetRatio>(natives.set_ratio)(clock, last_ratio);
  if (report) {
    report("WORLD CLOCK speed %.3f (after the sky took the new hour)", double(last_ratio));
  }
}

}  // namespace kcds::runtime::world
