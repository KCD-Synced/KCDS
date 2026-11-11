#include "../view_steps.hpp"

#include "kcd2s/testkit.hpp"

namespace steps = kcds::runtime::camera::view_steps;

KCD2S_TEST(view_steps, the_key_goes_close_farther_farthest_and_back_to_the_first_person) {
  int step = 0;
  step = steps::next(step, true);
  KCD2S_CHECK_EQ(step, 1);
  step = steps::next(step, true);
  KCD2S_CHECK_EQ(step, 2);
  step = steps::next(step, true);
  KCD2S_CHECK_EQ(step, 3);
  step = steps::next(step, true);
  KCD2S_CHECK_EQ(step, 0);
  // Each step stands farther back than the one before.
  KCD2S_CHECK(steps::distance(1) < steps::distance(2));
  KCD2S_CHECK(steps::distance(2) < steps::distance(3));
}

KCD2S_TEST(view_steps, a_forced_third_person_steps_through_the_distances_only) {
  int step = 1;
  step = steps::next(step, false);
  step = steps::next(step, false);
  KCD2S_CHECK_EQ(step, 3);
  step = steps::next(step, false);
  KCD2S_CHECK_EQ(step, 1);
  // The third person forced from the first person starts close.
  KCD2S_CHECK_EQ(steps::next(0, false), 1);
}

KCD2S_TEST(view_steps, the_third_person_without_the_key_stands_at_the_usual_distance) {
  const float usual = steps::distance(steps::kUsualStep);
  KCD2S_CHECK(steps::distance(0) == usual);
  KCD2S_CHECK(steps::distance(9) == usual);
  KCD2S_CHECK(steps::distance(-1) == usual);
  // The usual distance is the one the camera always had.
  KCD2S_CHECK(usual == 2.4f);
}
