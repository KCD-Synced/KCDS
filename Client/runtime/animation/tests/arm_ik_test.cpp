#include "../arm_ik.hpp"

#include <cmath>

#include "kcd2s/testkit.hpp"

namespace ik = kcds::runtime::animation::arm_ik;

namespace {

bool near(float a, float b, float tolerance = 1e-3f) {
  return std::abs(a - b) <= tolerance;
}

// The hand after the elbow bends by `angle` around `axis` (the forearm turned about the elbow).
void bent_hand(const float elbow[3], const float hand[3], const float axis[3], float angle,
               float out[3]) {
  float q[4];
  ik::axis_angle(axis, angle, q);
  float lower[3];
  ik::subtract(hand, elbow, lower);
  float turned[3];
  ik::rotate(q, lower, turned);
  for (int i = 0; i < 3; ++i) {
    out[i] = elbow[i] + turned[i];
  }
}

}  // namespace

KCD2S_TEST(arm_ik, the_elbow_bends_to_the_targets_distance) {
  const float shoulder[3] = {0, 0, 0};
  const float elbow[3] = {1, 0, 0};
  const float hand[3] = {1, 1, 0};  // bent at a right angle: the hand sqrt(2) from the shoulder
  const float target[3] = {0, 1.8f, 0};
  float axis[3];
  const float change = ik::bend_change(shoulder, elbow, hand, target, axis);
  float moved[3];
  bent_hand(elbow, hand, axis, change, moved);
  KCD2S_CHECK(near(ik::length(moved), 1.8f));
}

KCD2S_TEST(arm_ik, a_target_out_of_reach_leaves_the_arm_nearly_straight) {
  const float shoulder[3] = {0, 0, 0};
  const float elbow[3] = {1, 0, 0};
  const float hand[3] = {1, 1, 0};
  const float target[3] = {5, 5, 0};
  float axis[3];
  const float change = ik::bend_change(shoulder, elbow, hand, target, axis);
  float moved[3];
  bent_hand(elbow, hand, axis, change, moved);
  KCD2S_CHECK(near(ik::length(moved), 0.999f * 2.0f));
}

KCD2S_TEST(arm_ik, the_shoulder_turns_the_hand_onto_the_target) {
  const float from[3] = {1, 0, 0};
  const float to[3] = {0, 0, 3};
  float q[4];
  ik::rotation_between(from, to, q);
  float turned[3];
  ik::rotate(q, from, turned);
  KCD2S_CHECK(near(turned[0], 0) && near(turned[1], 0) && near(turned[2], 1));
}

KCD2S_TEST(arm_ik, blend_goes_from_one_rotation_to_the_other) {
  const float none[4] = {0, 0, 0, 1};
  const float axis[3] = {0, 0, 1};
  float quarter[4];
  ik::axis_angle(axis, 1.5707964f, quarter);
  float out[4];
  ik::blend(none, quarter, 0.0f, out);
  KCD2S_CHECK(near(out[3], 1));
  ik::blend(none, quarter, 1.0f, out);
  KCD2S_CHECK(near(out[2], quarter[2]) && near(out[3], quarter[3]));
}

KCD2S_TEST(arm_ik, the_twist_of_a_turn_about_the_axis_is_the_turn) {
  const float axis[3] = {1, 0, 0};
  float turn[4];
  ik::axis_angle(axis, 0.7f, turn);
  float twist[4];
  KCD2S_CHECK(ik::twist_around(turn, axis, twist));
  for (int i = 0; i < 4; ++i) {
    KCD2S_CHECK(near(twist[i], turn[i]));
  }
}

KCD2S_TEST(arm_ik, the_twist_leaves_out_the_swing_across_the_axis) {
  // A forearm along x: the hand twists 40 degrees about it, then bends 30 degrees about z.
  const float along[3] = {1, 0, 0};
  const float across[3] = {0, 0, 1};
  float twist_in[4];
  float swing[4];
  ik::axis_angle(along, 0.6981317f, twist_in);
  ik::axis_angle(across, 0.5235988f, swing);
  // swing * twist
  const float q[4] = {swing[3] * twist_in[0] + swing[0] * twist_in[3] + swing[1] * twist_in[2] -
                          swing[2] * twist_in[1],
                      swing[3] * twist_in[1] - swing[0] * twist_in[2] + swing[1] * twist_in[3] +
                          swing[2] * twist_in[0],
                      swing[3] * twist_in[2] + swing[0] * twist_in[1] - swing[1] * twist_in[0] +
                          swing[2] * twist_in[3],
                      swing[3] * twist_in[3] - swing[0] * twist_in[0] - swing[1] * twist_in[1] -
                          swing[2] * twist_in[2]};
  float twist[4];
  KCD2S_CHECK(ik::twist_around(q, along, twist));
  for (int i = 0; i < 4; ++i) {
    KCD2S_CHECK(near(twist[i], twist_in[i]));
  }
}

KCD2S_TEST(arm_ik, a_half_turn_across_the_axis_has_no_twist) {
  const float axis[3] = {0, 0, 1};
  const float half_turn_about_x[4] = {1, 0, 0, 0};
  float twist[4];
  KCD2S_CHECK(!ik::twist_around(half_turn_about_x, axis, twist));
}
