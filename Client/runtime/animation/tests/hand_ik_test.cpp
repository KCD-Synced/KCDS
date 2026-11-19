#include "../hand_ik.hpp"

#include <array>
#include <cmath>
#include <vector>

#include "kcd2s/testkit.hpp"

namespace ik = kcds::runtime::animation::hand_ik;

namespace {

// A body's root, a shoulder, the upper arm hanging down 0.3 m, a forearm roll joint, the hand
// 0.25 m below the elbow and its grip slot in front of the palm.
struct Arm {
  std::vector<ik::QuatT> relative;
  std::vector<ik::QuatT> absolute;
  std::vector<std::uint16_t> parents{ik::kNoParent, 0, 1, 2, 3, 4};
  ik::Pose pose;
  ik::Chain chain;

  Arm() {
    const float points[6][3] = {{0, 0, 0},      {0.2f, 0, 1.4f}, {0, 0, -0.3f},
                                {0, 0, -0.12f}, {0, 0, -0.13f},  {0, 0.05f, -0.08f}};
    for (const auto& point : points) {
      relative.push_back({{0, 0, 0, 1}, {point[0], point[1], point[2]}});
    }
    absolute.resize(relative.size());
    absolute[0] = relative[0];
    for (std::size_t joint = 1; joint < relative.size(); ++joint) {
      absolute[joint] = ik::compose(absolute[parents[joint]], relative[joint]);
    }
    pose = {relative.data(), absolute.data(), parents.data(), int(relative.size())};
    // The forearm is joint 2 (its parent, 1, the upper arm), the roll joint 3, the hand 4.
    found = ik::find_chain(parents.data(), int(parents.size()), 2, 4, 5, chain);
  }

  bool found = false;

  float distance(int a, int b) const {
    return std::hypot(absolute[a].t[0] - absolute[b].t[0], absolute[a].t[1] - absolute[b].t[1],
                      absolute[a].t[2] - absolute[b].t[2]);
  }
};

bool near(const float* a, const float* b, int n, float tolerance) {
  for (int i = 0; i < n; ++i) {
    if (std::abs(a[i] - b[i]) > tolerance) {
      return false;
    }
  }
  return true;
}

}  // namespace

KCD2S_TEST(hand_ik, the_grip_reaches_the_target_and_the_bones_keep_their_length) {
  Arm arm;
  KCD2S_CHECK(arm.found);
  KCD2S_CHECK_EQ(arm.chain.upper, 1);
  KCD2S_CHECK_EQ(arm.chain.grip, 5);
  const float upper = arm.distance(1, 2);
  const float lower = arm.distance(2, 4);
  const float hand_before[4] = {arm.absolute[4].q[0], arm.absolute[4].q[1], arm.absolute[4].q[2],
                                arm.absolute[4].q[3]};
  ik::Goal goal;
  goal.position[0] = 0.25f;
  goal.position[1] = 0.35f;
  goal.position[2] = 1.25f;
  KCD2S_CHECK(ik::solve(arm.pose, arm.chain, goal));
  KCD2S_CHECK(near(arm.absolute[5].t, goal.position, 3, 0.002f));
  KCD2S_CHECK(std::abs(arm.distance(1, 2) - upper) < 1e-4f);
  KCD2S_CHECK(std::abs(arm.distance(2, 4) - lower) < 1e-4f);
  // The hand keeps its turn in the body: a held cup stays upright.
  KCD2S_CHECK(near(arm.absolute[4].q, hand_before, 4, 1e-4f));
  KCD2S_CHECK(arm.absolute[1].t[2] == 1.4f);  // the shoulder stays
}

KCD2S_TEST(hand_ik, a_given_rotation_turns_the_hand_and_the_grip_still_lands) {
  Arm arm;
  ik::Goal goal;
  goal.position[0] = 0.2f;
  goal.position[1] = 0.4f;
  goal.position[2] = 1.3f;
  goal.has_rotation = true;
  // A quarter turn about the up axis.
  goal.rotation[2] = std::sin(0.785398f);
  goal.rotation[3] = std::cos(0.785398f);
  KCD2S_CHECK(ik::solve(arm.pose, arm.chain, goal));
  KCD2S_CHECK(near(arm.absolute[4].q, goal.rotation, 4, 1e-4f));
  KCD2S_CHECK(near(arm.absolute[5].t, goal.position, 3, 0.002f));
}

KCD2S_TEST(hand_ik, the_pole_takes_the_elbow_to_its_side) {
  Arm out;
  Arm in;
  ik::Goal goal;
  goal.position[0] = 0.2f;
  goal.position[1] = 0.3f;
  goal.position[2] = 1.3f;
  goal.has_pole = true;
  goal.pole[0] = 1.0f;
  goal.pole[2] = 1.3f;
  KCD2S_CHECK(ik::solve(out.pose, out.chain, goal));
  goal.pole[0] = -1.0f;
  KCD2S_CHECK(ik::solve(in.pose, in.chain, goal));
  KCD2S_CHECK(out.absolute[2].t[0] > 0.25f);
  KCD2S_CHECK(in.absolute[2].t[0] < 0.15f);
  KCD2S_CHECK(near(out.absolute[5].t, goal.position, 3, 0.002f));
  KCD2S_CHECK(near(in.absolute[5].t, goal.position, 3, 0.002f));
}

KCD2S_TEST(hand_ik, weight_blends_and_a_target_out_of_reach_stretches_toward_it) {
  Arm still;
  ik::Goal none;
  none.position[1] = 0.4f;
  none.position[2] = 1.3f;
  none.weight = 0.0f;
  const ik::QuatT grip = still.absolute[5];
  KCD2S_CHECK(ik::solve(still.pose, still.chain, none));
  KCD2S_CHECK(near(still.absolute[5].t, grip.t, 3, 1e-6f));

  Arm half;
  ik::Goal goal;
  goal.position[0] = 0.2f;
  goal.position[1] = 0.45f;
  goal.position[2] = 1.3f;
  goal.weight = 0.5f;
  KCD2S_CHECK(ik::solve(half.pose, half.chain, goal));
  const float moved =
      std::hypot(half.absolute[5].t[0] - grip.t[0], half.absolute[5].t[1] - grip.t[1],
                 half.absolute[5].t[2] - grip.t[2]);
  const float left =
      std::hypot(half.absolute[5].t[0] - goal.position[0], half.absolute[5].t[1] - goal.position[1],
                 half.absolute[5].t[2] - goal.position[2]);
  KCD2S_CHECK(moved > 0.05f && left > 0.05f);

  Arm far;
  ik::Goal reach;
  reach.position[0] = 0.2f;
  reach.position[1] = 2.0f;
  reach.position[2] = 1.4f;
  KCD2S_CHECK(ik::solve(far.pose, far.chain, reach));
  // Straight out ahead, as far as the arm goes.
  KCD2S_CHECK(far.absolute[4].t[1] > 0.5f);
  KCD2S_CHECK(std::abs(far.absolute[4].t[0] - 0.2f) < 0.01f);
}

KCD2S_TEST(hand_ik, an_arm_the_skeleton_lacks_is_no_chain) {
  const std::vector<std::uint16_t> parents{ik::kNoParent, 0, 1};
  ik::Chain chain;
  KCD2S_CHECK(!ik::find_chain(parents.data(), 3, 0, 2, -1, chain));  // forearm without a parent
  KCD2S_CHECK(!ik::find_chain(parents.data(), 3, 2, 1, -1, chain));  // hand above the forearm
  KCD2S_CHECK(ik::find_chain(parents.data(), 3, 1, 2, -1, chain));
  KCD2S_CHECK_EQ(chain.grip, 2);  // no slot: the hand itself
}
