// A horse passenger's pose: upright, the hands at the rider's waist (riding/passenger_hold.hpp).
#include "../passenger_hold.hpp"

#include <cmath>
#include <vector>

#include "kcd2s/testkit.hpp"

namespace hold = kcds::runtime::riding::passenger_hold;
using hold::Joint;

namespace {

constexpr float kPi = 3.14159265358979f;

void multiply(const float a[4], const float b[4], float out[4]) {
  out[0] = a[3] * b[0] + b[3] * a[0] + (a[1] * b[2] - a[2] * b[1]);
  out[1] = a[3] * b[1] + b[3] * a[1] + (a[2] * b[0] - a[0] * b[2]);
  out[2] = a[3] * b[2] + b[3] * a[2] + (a[0] * b[1] - a[1] * b[0]);
  out[3] = a[3] * b[3] - (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]);
}

void rotate(const float q[4], const float v[3], float out[3]) {
  const float p[4] = {v[0], v[1], v[2], 0.0f};
  const float inverse[4] = {-q[0], -q[1], -q[2], q[3]};
  float a[4];
  float b[4];
  multiply(q, p, a);
  multiply(a, inverse, b);
  out[0] = b[0];
  out[1] = b[1];
  out[2] = b[2];
}

Joint compose(const Joint& parent, const Joint& child) {
  Joint out{};
  multiply(parent.q, child.q, out.q);
  float moved[3];
  rotate(parent.q, child.t, moved);
  for (int i = 0; i < 3; ++i) {
    out.t[i] = moved[i] + parent.t[i];
  }
  return out;
}

Joint at(float x, float y, float z) {
  return Joint{{0.0f, 0.0f, 0.0f, 1.0f}, {x, y, z}};
}

// A rider's sitting pose in small: hips, a spine leaning 30 degrees forward, a neck, and two arms
// hanging from the shoulders.
struct Pose {
  hold::Skeleton skeleton;
  std::vector<Joint> relative;
  std::vector<Joint> absolute;
};

Pose rider_pose() {
  Pose pose;
  // 0 hips, 1 spine, 2 neck, then per side shoulder, upper arm, forearm, hand.
  pose.skeleton.parents = {hold::kNoParent, 0, 1, 1, 3, 4, 5, 1, 7, 8, 9};
  pose.relative = {at(0, 0, 1.0f),   at(0, 0, 0.1f),   at(0, 0, 0.5f),   at(-0.1f, 0, 0.45f),
                   at(-0.08f, 0, 0), at(0, 0, -0.30f), at(0, 0, -0.28f), at(0.1f, 0, 0.45f),
                   at(0.08f, 0, 0),  at(0, 0, -0.30f), at(0, 0, -0.28f)};
  const float lean = -30.0f * kPi / 180.0f;
  pose.relative[1].q[0] = std::sin(lean / 2.0f);
  pose.relative[1].q[3] = std::cos(lean / 2.0f);
  pose.absolute.resize(pose.relative.size());
  for (std::size_t joint = 0; joint < pose.relative.size(); ++joint) {
    const auto parent = pose.skeleton.parents[joint];
    pose.absolute[joint] = parent == hold::kNoParent
                               ? pose.relative[joint]
                               : compose(pose.absolute[parent], pose.relative[joint]);
  }
  pose.skeleton.hips = 0;
  pose.skeleton.spine = 1;
  pose.skeleton.neck = 2;
  pose.skeleton.upper_arm[0] = 4;
  pose.skeleton.forearm[0] = 5;
  pose.skeleton.hand[0] = 6;
  pose.skeleton.upper_arm[1] = 8;
  pose.skeleton.forearm[1] = 9;
  pose.skeleton.hand[1] = 10;
  return pose;
}

float distance(const float a[3], const float b[3]) {
  return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) +
                   (a[2] - b[2]) * (a[2] - b[2]));
}

}  // namespace

KCD2S_TEST(passenger_hold, sits_upright_with_the_hands_at_the_riders_waist) {
  Pose pose = rider_pose();
  KCD2S_CHECK(hold::prepare(pose.skeleton));
  KCD2S_CHECK(hold::apply(pose.skeleton, pose.relative.data(), pose.absolute.data(), 1.0f));
  // The spine leans only kLeanDegrees forward.
  const float* spine = pose.absolute[1].t;
  const float* neck = pose.absolute[2].t;
  const float up[3] = {neck[0] - spine[0], neck[1] - spine[1], neck[2] - spine[2]};
  const float lean = std::atan2(up[1], up[2]) * 180.0f / kPi;
  KCD2S_CHECK(std::fabs(lean - hold::kLeanDegrees) < 0.5f);
  KCD2S_CHECK(std::fabs(up[0]) < 1e-3f);
  // Each hand at the rider's side.
  const float* hips = pose.absolute[0].t;
  for (int side = 0; side < 2; ++side) {
    const float sign = side == 0 ? -1.0f : 1.0f;
    const float wanted[3] = {hips[0] + sign * hold::kHandSide, hips[1] + hold::kHandAhead,
                             hips[2] + hold::kHandUp};
    KCD2S_CHECK(distance(pose.absolute[std::size_t(pose.skeleton.hand[side])].t, wanted) < 0.01f);
  }
  // The joints relative to their parents give the same pose.
  for (std::size_t joint = 1; joint < pose.relative.size(); ++joint) {
    const Joint again = compose(pose.absolute[pose.skeleton.parents[joint]], pose.relative[joint]);
    KCD2S_CHECK(distance(again.t, pose.absolute[joint].t) < 1e-4f);
  }
}

KCD2S_TEST(passenger_hold, leans_as_the_rider_does_and_holds_on_to_them) {
  Pose pose = rider_pose();
  KCD2S_CHECK(hold::prepare(pose.skeleton));
  hold::Rider rider;
  rider.known = true;
  // The rider leans back 15 degrees, braking, their waist come nearer.
  const float back = 15.0f * kPi / 180.0f;
  rider.spine[1] = -std::sin(back);
  rider.spine[2] = std::cos(back);
  const float* hips = pose.absolute[0].t;
  for (int side = 0; side < 2; ++side) {
    const float sign = side == 0 ? -1.0f : 1.0f;
    rider.hands[side][0] = hips[0] + sign * 0.18f;
    rider.hands[side][1] = hips[1] + 0.28f;
    rider.hands[side][2] = hips[2] + 0.2f;
  }
  KCD2S_CHECK(hold::apply(pose.skeleton, pose.relative.data(), pose.absolute.data(), 1.0f, rider));
  const float* spine = pose.absolute[1].t;
  const float* neck = pose.absolute[2].t;
  const float lean = std::atan2(neck[1] - spine[1], neck[2] - spine[2]) * 180.0f / kPi;
  KCD2S_CHECK(std::fabs(lean + 15.0f) < 0.5f);
  for (int side = 0; side < 2; ++side) {
    KCD2S_CHECK(distance(pose.absolute[std::size_t(pose.skeleton.hand[side])].t,
                         rider.hands[side]) < 0.01f);
  }
}

KCD2S_TEST(passenger_hold, a_rider_out_of_reach_or_lying_is_not_followed) {
  Pose pose = rider_pose();
  KCD2S_CHECK(hold::prepare(pose.skeleton));
  hold::Rider rider;
  rider.known = true;
  // Thrown off: lying along the ground, two metres away.
  rider.spine[1] = 1.0f;
  const float* hips = pose.absolute[0].t;
  for (int side = 0; side < 2; ++side) {
    rider.hands[side][0] = hips[0];
    rider.hands[side][1] = hips[1] + 2.0f;
    rider.hands[side][2] = 0.0f;
  }
  KCD2S_CHECK(hold::apply(pose.skeleton, pose.relative.data(), pose.absolute.data(), 1.0f, rider));
  const float* spine = pose.absolute[1].t;
  const float* neck = pose.absolute[2].t;
  const float lean = std::atan2(neck[1] - spine[1], neck[2] - spine[2]) * 180.0f / kPi;
  KCD2S_CHECK(std::fabs(lean - hold::kLeanDegrees) < 0.5f);
  for (int side = 0; side < 2; ++side) {
    const float sign = side == 0 ? -1.0f : 1.0f;
    const float wanted[3] = {hips[0] + sign * hold::kHandSide, hips[1] + hold::kHandAhead,
                             hips[2] + hold::kHandUp};
    KCD2S_CHECK(distance(pose.absolute[std::size_t(pose.skeleton.hand[side])].t, wanted) < 0.01f);
  }
}

KCD2S_TEST(passenger_hold, comes_in_by_its_weight) {
  Pose clip = rider_pose();
  Pose none = rider_pose();
  Pose half = rider_pose();
  KCD2S_CHECK(hold::prepare(none.skeleton) && hold::prepare(half.skeleton));
  // No weight leaves the clip's pose.
  KCD2S_CHECK(!hold::apply(none.skeleton, none.relative.data(), none.absolute.data(), 0.0f));
  KCD2S_CHECK(distance(none.absolute[6].t, clip.absolute[6].t) < 1e-6f);
  // Half the weight: the hand part of the way, the spine half upright.
  KCD2S_CHECK(hold::apply(half.skeleton, half.relative.data(), half.absolute.data(), 0.5f));
  const float* spine = half.absolute[1].t;
  const float* neck = half.absolute[2].t;
  const float lean = std::atan2(neck[1] - spine[1], neck[2] - spine[2]) * 180.0f / kPi;
  KCD2S_CHECK(lean > 10.0f && lean < 25.0f);
}

KCD2S_TEST(passenger_hold, a_skeleton_without_the_joints_is_left_alone) {
  Pose pose = rider_pose();
  pose.skeleton.hand[1] = -1;
  KCD2S_CHECK(!hold::prepare(pose.skeleton));
  KCD2S_CHECK(!hold::apply(pose.skeleton, pose.relative.data(), pose.absolute.data(), 1.0f));
}
