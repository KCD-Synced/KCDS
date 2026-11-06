#include "fall_knockout.hpp"

#include "kcd2s/testkit.hpp"

using kcds::runtime::knockout::FallKnockout;

KCD2S_TEST(fall_knockout, a_landing_that_takes_the_last_health_counts_once) {
  FallKnockout fall;
  KCD2S_CHECK(!fall.update(1000, false, 100.0f));
  KCD2S_CHECK(!fall.update(1100, true, 100.0f));
  KCD2S_CHECK(!fall.update(2000, true, 100.0f));
  KCD2S_CHECK(fall.update(2100, false, 1.0f));
  KCD2S_CHECK(!fall.update(2200, false, 1.0f));
}

KCD2S_TEST(fall_knockout, a_landing_that_leaves_health_is_no_knock_out) {
  FallKnockout fall;
  fall.update(1000, true, 100.0f);
  KCD2S_CHECK(!fall.update(1100, false, 40.0f));
}

KCD2S_TEST(fall_knockout, last_health_on_the_ground_is_left_alone) {
  FallKnockout fall;
  fall.update(1000, false, 100.0f);
  KCD2S_CHECK(!fall.update(1100, false, 1.0f));
  // Long after a jump, too.
  FallKnockout later;
  later.update(1000, true, 100.0f);
  later.update(1100, false, 100.0f);
  KCD2S_CHECK(!later.update(1100 + FallKnockout::kLandingMs + 1, false, 1.0f));
}

KCD2S_TEST(fall_knockout, a_small_hop_with_the_last_health_is_no_new_knock_out) {
  FallKnockout fall;
  fall.update(1000, false, 1.0f);
  fall.update(1100, true, 1.0f);
  KCD2S_CHECK(!fall.update(1200, false, 1.0f));
}
