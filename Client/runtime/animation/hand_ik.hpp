// A hand held at a target (Player.setHandTarget): the two-bone solve of an arm in a pose, on top of
// what the body plays. The upper arm turns so the elbow lies where the two bones reach the target
// (in the plane of the animation's own bend, or toward a pole), the forearm turns so the wrist
// reaches it, and the hand keeps its turn in the body (a vessel stays upright) unless one is
// given. The target is the grip: the hand slot's point (where held items attach) when the
// skeleton has one, so the wrist goes where that point lands it. Joints between the forearm and
// the hand (a roll joint) follow the forearm.
//
// Poses as the game's pose modifiers see them: QuatT (rotation x, y, z, w, then the position) per
// joint, relative to the parent and absolute in the character's space, with each joint's parent.
//
// Portable: the unit test builds it.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "arm_ik.hpp"

namespace kcds::runtime::animation::hand_ik {

struct QuatT {
  float q[4];
  float t[3];
};
static_assert(sizeof(QuatT) == 28, "the engine's QuatT is 28 bytes");

inline constexpr std::uint16_t kNoParent = 0xFFFF;

struct Pose {
  QuatT* relative = nullptr;
  QuatT* absolute = nullptr;
  const std::uint16_t* parents = nullptr;
  int count = 0;
};

// The arm's joints and the joints under its upper arm, a parent before its children.
struct Chain {
  int upper = -1;
  int fore = -1;
  int hand = -1;
  int grip = -1;
  std::vector<int> below;
};

struct Goal {
  float position[3] = {};
  bool has_rotation = false;
  float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  bool has_pole = false;
  float pole[3] = {};
  float weight = 1.0f;
};

inline void multiply(const float a[4], const float b[4], float out[4]) {
  const float x = a[3] * b[0] + b[3] * a[0] + (a[1] * b[2] - a[2] * b[1]);
  const float y = a[3] * b[1] + b[3] * a[1] + (a[2] * b[0] - a[0] * b[2]);
  const float z = a[3] * b[2] + b[3] * a[2] + (a[0] * b[1] - a[1] * b[0]);
  const float w = a[3] * b[3] - (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]);
  out[0] = x;
  out[1] = y;
  out[2] = z;
  out[3] = w;
}

inline QuatT compose(const QuatT& parent, const QuatT& child) {
  QuatT out{};
  multiply(parent.q, child.q, out.q);
  float moved[3];
  arm_ik::rotate(parent.q, child.t, moved);
  out.t[0] = moved[0] + parent.t[0];
  out.t[1] = moved[1] + parent.t[1];
  out.t[2] = moved[2] + parent.t[2];
  return out;
}

// The chain of an arm by its forearm, hand and grip joints: the upper arm is the forearm's parent,
// the hand must hang from the forearm and the grip from the hand (or be the hand). False when the
// skeleton has no such arm.
inline bool find_chain(const std::uint16_t* parents, int count, int fore, int hand, int grip,
                       Chain& chain) {
  if (fore < 0 || fore >= count || hand < 0 || hand >= count) {
    return false;
  }
  const int upper = parents[fore];
  if (upper == kNoParent || upper >= count) {
    return false;
  }
  const auto hangs_from = [&](int joint, int ancestor) {
    for (int at = joint, depth = 0; at != kNoParent && depth <= count; at = parents[at], ++depth) {
      if (at == ancestor) {
        return true;
      }
    }
    return false;
  };
  if (!hangs_from(hand, fore)) {
    return false;
  }
  if (grip < 0 || grip >= count || !hangs_from(grip, hand)) {
    grip = hand;
  }
  std::vector<std::pair<int, int>> under;
  for (int joint = 0; joint < count; ++joint) {
    int depth = 0;
    for (int at = parents[joint]; at != kNoParent && depth <= count; at = parents[at]) {
      ++depth;
      if (at == upper) {
        under.emplace_back(depth, joint);
        break;
      }
    }
  }
  std::sort(under.begin(), under.end());
  chain.upper = upper;
  chain.fore = fore;
  chain.hand = hand;
  chain.grip = grip;
  chain.below.clear();
  for (const auto& [depth, joint] : under) {
    chain.below.push_back(joint);
  }
  return true;
}

// Turns `joint` to the absolute rotation `q` and places everything under the upper arm again.
inline void set_absolute(Pose& pose, const Chain& chain, int joint, const float q[4]) {
  const std::uint16_t parent = pose.parents[joint];
  if (parent != kNoParent) {
    const float* p = pose.absolute[parent].q;
    const float inverse[4] = {-p[0], -p[1], -p[2], p[3]};
    multiply(inverse, q, pose.relative[joint].q);
  } else {
    std::copy(q, q + 4, pose.relative[joint].q);
  }
  std::copy(q, q + 4, pose.absolute[joint].q);
  for (const int below : chain.below) {
    pose.absolute[below] = compose(pose.absolute[pose.parents[below]], pose.relative[below]);
  }
}

// Solves the arm toward the goal, blended into the pose by the goal's weight. False when the arm
// cannot be solved (bones of no length); the pose is then as it was.
inline bool solve(Pose& pose, const Chain& chain, const Goal& goal) {
  if (!(goal.weight > 0.0f)) {
    return true;
  }
  QuatT* abs = pose.absolute;
  const float* shoulder = abs[chain.upper].t;
  float upper_bone[3];
  float lower_bone[3];
  arm_ik::subtract(abs[chain.fore].t, shoulder, upper_bone);
  arm_ik::subtract(abs[chain.hand].t, abs[chain.fore].t, lower_bone);
  const float a = arm_ik::length(upper_bone);
  const float b = arm_ik::length(lower_bone);
  if (a < 1e-4f || b < 1e-4f) {
    return false;
  }
  float original[3][4];
  const int joints[3] = {chain.upper, chain.fore, chain.hand};
  for (int i = 0; i < 3; ++i) {
    std::copy(abs[joints[i]].q, abs[joints[i]].q + 4, original[i]);
  }
  // The hand's turn, and where the wrist goes so that the grip lands on the target.
  const float* turn_source = goal.has_rotation ? goal.rotation : abs[chain.hand].q;
  float hand_turn[4];
  std::copy(turn_source, turn_source + 4, hand_turn);
  float grip_offset[3];
  arm_ik::subtract(abs[chain.grip].t, abs[chain.hand].t, grip_offset);
  const float* h = abs[chain.hand].q;
  const float hand_inverse[4] = {-h[0], -h[1], -h[2], h[3]};
  float grip_in_hand[3];
  arm_ik::rotate(hand_inverse, grip_offset, grip_in_hand);
  float grip_now[3];
  arm_ik::rotate(hand_turn, grip_in_hand, grip_now);
  const float wrist[3] = {goal.position[0] - grip_now[0], goal.position[1] - grip_now[1],
                          goal.position[2] - grip_now[2]};
  // Where the elbow goes: in the plane of the reach and the animation's elbow (or the pole).
  float reach[3];
  arm_ik::subtract(wrist, shoulder, reach);
  const float reach_length = arm_ik::length(reach);
  if (reach_length < 1e-4f) {
    return false;
  }
  const float d = std::clamp(reach_length, std::abs(a - b) + 0.001f, 0.999f * (a + b));
  const float u[3] = {reach[0] / reach_length, reach[1] / reach_length, reach[2] / reach_length};
  float side[3];
  if (goal.has_pole) {
    arm_ik::subtract(goal.pole, shoulder, side);
  } else {
    side[0] = upper_bone[0];
    side[1] = upper_bone[1];
    side[2] = upper_bone[2];
  }
  const float along = arm_ik::dot(side, u);
  float v[3] = {side[0] - u[0] * along, side[1] - u[1] * along, side[2] - u[2] * along};
  float v_length = arm_ik::length(v);
  if (v_length < 1e-4f) {
    // The elbow in line with the reach: bend it down.
    const float down[3] = {0.0f, 0.0f, -1.0f};
    const float down_along = arm_ik::dot(down, u);
    v[0] = down[0] - u[0] * down_along;
    v[1] = down[1] - u[1] * down_along;
    v[2] = down[2] - u[2] * down_along;
    v_length = arm_ik::length(v);
    if (v_length < 1e-4f) {
      v[0] = 1.0f;
      v[1] = v[2] = 0.0f;
      v_length = 1.0f;
    }
  }
  v[0] /= v_length;
  v[1] /= v_length;
  v[2] /= v_length;
  const float cosine = std::clamp((a * a + d * d - b * b) / (2.0f * a * d), -1.0f, 1.0f);
  const float sine = std::sqrt(std::max(0.0f, 1.0f - cosine * cosine));
  const float elbow_goal[3] = {shoulder[0] + a * (u[0] * cosine + v[0] * sine),
                               shoulder[1] + a * (u[1] * cosine + v[1] * sine),
                               shoulder[2] + a * (u[2] * cosine + v[2] * sine)};
  const float wrist_goal[3] = {shoulder[0] + u[0] * d, shoulder[1] + u[1] * d,
                               shoulder[2] + u[2] * d};
  // The upper arm, then the forearm, then the hand.
  float to_elbow[3];
  arm_ik::subtract(elbow_goal, shoulder, to_elbow);
  float turn[4];
  arm_ik::rotation_between(upper_bone, to_elbow, turn);
  float solved[3][4];
  multiply(turn, original[0], solved[0]);
  set_absolute(pose, chain, chain.upper, solved[0]);
  float forearm_now[3];
  float forearm_goal[3];
  arm_ik::subtract(abs[chain.hand].t, abs[chain.fore].t, forearm_now);
  arm_ik::subtract(wrist_goal, abs[chain.fore].t, forearm_goal);
  arm_ik::rotation_between(forearm_now, forearm_goal, turn);
  multiply(turn, abs[chain.fore].q, solved[1]);
  set_absolute(pose, chain, chain.fore, solved[1]);
  std::copy(hand_turn, hand_turn + 4, solved[2]);
  set_absolute(pose, chain, chain.hand, solved[2]);
  if (goal.weight < 1.0f) {
    for (int i = 0; i < 3; ++i) {
      float mixed[4];
      arm_ik::blend(original[i], solved[i], goal.weight, mixed);
      set_absolute(pose, chain, joints[i], mixed);
    }
  }
  return true;
}

}  // namespace kcds::runtime::animation::hand_ik
