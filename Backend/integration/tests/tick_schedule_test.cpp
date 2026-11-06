// Fixed-rate tick schedule of the local server loop (phase 1).
#include "../local/tick/fixed_rate_schedule.hpp"

#include <cmath>
#include <cstdint>

#include "kcd2s/testkit.hpp"

using kcd2s::local::tick::FixedRateSchedule;
using kcd2s::local::tick::parse_tick_rate;
using kcd2s::local::tick::TickSlot;

namespace {
bool close_to(double a, double b) {
  return std::fabs(a - b) < 1e-9;
}
}  // namespace

KCD2S_TEST(tick_schedule, parses_tick_rate) {
  KCD2S_CHECK(!parse_tick_rate(nullptr).has_value());
  KCD2S_CHECK(!parse_tick_rate("").has_value());
  KCD2S_CHECK(!parse_tick_rate("fast").has_value());
  KCD2S_CHECK(!parse_tick_rate("60hz").has_value());
  KCD2S_CHECK(!parse_tick_rate("5").has_value());
  KCD2S_CHECK(!parse_tick_rate("1000").has_value());
  KCD2S_CHECK(!parse_tick_rate("nan").has_value());
  KCD2S_CHECK(close_to(*parse_tick_rate("60"), 60.0));
  KCD2S_CHECK(close_to(*parse_tick_rate("30.5"), 30.5));
  KCD2S_CHECK(close_to(*parse_tick_rate("240"), 240.0));
}

KCD2S_TEST(tick_schedule, on_time_ticks_follow_absolute_deadlines) {
  FixedRateSchedule schedule(50.0);  // 20 ms
  schedule.start(10.0);
  KCD2S_CHECK(close_to(schedule.next_deadline(), 10.0));
  for (int i = 0; i < 5; ++i) {
    const double deadline = 10.0 + i * 0.02;
    const TickSlot slot = schedule.begin_tick(deadline);
    KCD2S_CHECK(close_to(slot.deadline, deadline));
    KCD2S_CHECK(close_to(slot.lateness, 0.0));
    KCD2S_CHECK_EQ(slot.skipped, std::uint64_t{0});
  }
  KCD2S_CHECK(close_to(schedule.next_deadline(), 10.1));
}

KCD2S_TEST(tick_schedule, late_start_keeps_the_cadence) {
  FixedRateSchedule schedule(50.0);
  schedule.start(0.0);
  schedule.begin_tick(0.0);
  // Woke 7 ms late for the 20 ms deadline: the next deadline is still 40 ms, not 47 ms.
  const TickSlot slot = schedule.begin_tick(0.027);
  KCD2S_CHECK(close_to(slot.deadline, 0.02));
  KCD2S_CHECK(close_to(slot.lateness, 0.007));
  KCD2S_CHECK_EQ(slot.skipped, std::uint64_t{0});
  KCD2S_CHECK(close_to(schedule.next_deadline(), 0.04));
}

KCD2S_TEST(tick_schedule, falling_behind_skips_whole_slots_without_burst) {
  FixedRateSchedule schedule(50.0);
  schedule.start(0.0);
  schedule.begin_tick(0.0);
  // The previous tick overran: the loop is ready again at 75 ms. Deadlines 20 and 40 ms are
  // gone; this tick takes the 60 ms slot and two slots are reported as skipped.
  const TickSlot slot = schedule.begin_tick(0.075);
  KCD2S_CHECK_EQ(slot.skipped, std::uint64_t{2});
  KCD2S_CHECK(close_to(slot.deadline, 0.06));
  KCD2S_CHECK(close_to(slot.lateness, 0.015));
  KCD2S_CHECK(close_to(schedule.next_deadline(), 0.08));
  KCD2S_CHECK_EQ(schedule.skipped_total(), std::uint64_t{2});
  KCD2S_CHECK_EQ(schedule.ticks(), std::uint64_t{2});
}

KCD2S_TEST(tick_schedule, early_wake_is_clamped_to_its_slot) {
  FixedRateSchedule schedule(60.0);
  schedule.start(1.0);
  const TickSlot slot = schedule.begin_tick(0.9999);  // timer resolution woke marginally early
  KCD2S_CHECK(close_to(slot.deadline, 1.0));
  KCD2S_CHECK(close_to(slot.lateness, 0.0));
  KCD2S_CHECK(close_to(schedule.next_deadline(), 1.0 + 1.0 / 60.0));
}

KCD2S_TEST(tick_schedule, jittery_wakeups_still_achieve_the_rate) {
  // Simulate a loop that always wakes 0 to 1.5 ms after its deadline (timer jitter) and works
  // 4 ms per tick. Over 600 ticks the achieved rate must be the configured 60 Hz.
  FixedRateSchedule schedule(60.0);
  schedule.start(0.0);
  double now = 0.0;
  std::uint32_t jitter = 12345;
  double first_start = -1.0;
  double last_start = 0.0;
  for (int i = 0; i < 600; ++i) {
    jitter = jitter * 1103515245u + 12345u;
    const double wake_error = double((jitter >> 16) % 1500u) * 1e-6;
    if (now < schedule.next_deadline())
      now = schedule.next_deadline();
    now += wake_error;
    const TickSlot slot = schedule.begin_tick(now);
    KCD2S_CHECK_EQ(slot.skipped, std::uint64_t{0});
    if (first_start < 0.0)
      first_start = now;
    last_start = now;
    now += 0.004;  // tick work
  }
  const double achieved_hz = 599.0 / (last_start - first_start);
  KCD2S_CHECK(std::fabs(achieved_hz - 60.0) < 0.2);
  KCD2S_CHECK_EQ(schedule.skipped_total(), std::uint64_t{0});
}
