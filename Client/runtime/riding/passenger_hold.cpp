#include "passenger_hold.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "../animation/arm_ik.hpp"

namespace kcds::runtime::riding::passenger_hold {
namespace {

namespace ik = kcds::runtime::animation::arm_ik;

constexpr float kPi = 3.14159265358979f;
// How far a straight arm's elbow bends before the reach (radians).
constexpr float kStartBend = 0.2f;
// The rider's side is held only as far as this share of the arm's length, and the rider's lean
// copied only up to this many degrees from upright.
constexpr float kMostReach = 0.95f;
constexpr float kMostRiderLean = 35.0f;

// a * b.
void multiply(const float a[4], const float b[4], float out[4]) {
  const float x = a[3] * b[0] + b[3] * a[0] + (a[1] * b[2] - a[2] * b[1]);
  const float y = a[3] * b[1] + b[3] * a[1] + (a[2] * b[0] - a[0] * b[2]);
  const float z = a[3] * b[2] + b[3] * a[2] + (a[0] * b[1] - a[1] * b[0]);
  const float w = a[3] * b[3] - (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]);
  out[0] = x;
  out[1] = y;
  out[2] = z;
  out[3] = w;
}

// parent * child: the child's joint in the parent's space.
Joint compose(const Joint& parent, const Joint& child) {
  Joint out{};
  multiply(parent.q, child.q, out.q);
  float moved[3];
  ik::rotate(parent.q, child.t, moved);
  out.t[0] = moved[0] + parent.t[0];
  out.t[1] = moved[1] + parent.t[1];
  out.t[2] = moved[2] + parent.t[2];
  return out;
}

// Turns `joint` by the rotation `turn` (in the character's space) about its own place, and moves
// everything under it along.
void turn_joint(const Skeleton& skeleton, Joint* relative, Joint* absolute, int joint,
                const float turn[4], const std::vector<int>& below) {
  float turned[4];
  multiply(turn, absolute[joint].q, turned);
  const std::uint16_t parent = skeleton.parents[std::size_t(joint)];
  if (parent != kNoParent) {
    const float* p = absolute[parent].q;
    const float inverse[4] = {-p[0], -p[1], -p[2], p[3]};
    multiply(inverse, turned, relative[joint].q);
  } else {
    std::copy(turned, turned + 4, relative[joint].q);
  }
  std::copy(turned, turned + 4, absolute[joint].q);
  for (const int child : below) {
    absolute[child] = compose(absolute[skeleton.parents[std::size_t(child)]], relative[child]);
  }
}

// `weight` of the way from no turn to `full`.
void weighted(const float full[4], float weight, float out[4]) {
  const float none[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  ik::blend(none, full, weight, out);
}

// Whether the hand can get to `target` with the arm as long as it is (from the upper arm through
// the elbow), a little short of straight.
bool within_reach(const Skeleton& skeleton, const Joint* absolute, int side,
                  const float target[3]) {
  const float* shoulder = absolute[skeleton.upper_arm[side]].t;
  const float* elbow = absolute[skeleton.forearm[side]].t;
  const float* hand = absolute[skeleton.hand[side]].t;
  float upper[3];
  float lower[3];
  float away[3];
  ik::subtract(elbow, shoulder, upper);
  ik::subtract(hand, elbow, lower);
  ik::subtract(target, shoulder, away);
  return ik::length(away) < kMostReach * (ik::length(upper) + ik::length(lower));
}

// One arm's reach for `target`.
void reach(const Skeleton& skeleton, Joint* relative, Joint* absolute, int side,
           const float target[3]) {
  const int upper = skeleton.upper_arm[side];
  const int fore = skeleton.forearm[side];
  const int hand = skeleton.hand[side];
  // An arm straight as a line has no plane to bend in: the elbow first bends a little forward,
  // as an elbow does (around the axis across the upper arm and the body's forward direction).
  float upper_dir[3];
  float lower_dir[3];
  ik::subtract(absolute[fore].t, absolute[upper].t, upper_dir);
  ik::subtract(absolute[hand].t, absolute[fore].t, lower_dir);
  float across[3];
  ik::cross(upper_dir, lower_dir, across);
  if (ik::length(across) < 1e-4f * ik::length(upper_dir) * ik::length(lower_dir)) {
    const float forward[3] = {0.0f, 1.0f, 0.0f};
    float bend_axis[3];
    ik::cross(upper_dir, forward, bend_axis);
    const float size = ik::length(bend_axis);
    if (size > 1e-5f) {
      bend_axis[0] /= size;
      bend_axis[1] /= size;
      bend_axis[2] /= size;
      float start[4];
      ik::axis_angle(bend_axis, kStartBend, start);
      turn_joint(skeleton, relative, absolute, fore, start, skeleton.below_forearm[side]);
    }
  }
  // The elbow bends to the target's distance from the shoulder.
  float axis[3];
  const float bend =
      ik::bend_change(absolute[upper].t, absolute[fore].t, absolute[hand].t, target, axis);
  if (bend != 0.0f) {
    float turn[4];
    ik::axis_angle(axis, bend, turn);
    turn_joint(skeleton, relative, absolute, fore, turn, skeleton.below_forearm[side]);
  }
  // The upper arm turns the hand onto it.
  float now[3];
  float wanted[3];
  ik::subtract(absolute[hand].t, absolute[upper].t, now);
  ik::subtract(target, absolute[upper].t, wanted);
  float turn[4];
  ik::rotation_between(now, wanted, turn);
  turn_joint(skeleton, relative, absolute, upper, turn, skeleton.below_upper_arm[side]);
}

}  // namespace

std::vector<int> joints_under(const std::vector<std::uint16_t>& parents, int joint) {
  const int count = int(parents.size());
  std::vector<std::pair<int, int>> under;
  for (int at = 0; at < count; ++at) {
    int depth = 0;
    for (int up = parents[std::size_t(at)]; up != kNoParent && depth <= count;
         up = parents[std::size_t(up)]) {
      ++depth;
      if (up == joint) {
        under.emplace_back(depth, at);
        break;
      }
      if (up >= count) {
        break;
      }
    }
  }
  std::sort(under.begin(), under.end());
  std::vector<int> joints;
  joints.reserve(under.size());
  for (const auto& [depth, at] : under) {
    joints.push_back(at);
  }
  return joints;
}

bool prepare(Skeleton& skeleton) {
  const int count = int(skeleton.parents.size());
  const auto valid = [count](int joint) { return joint >= 0 && joint < count; };
  if (!valid(skeleton.hips) || !valid(skeleton.spine) || !valid(skeleton.neck)) {
    return false;
  }
  for (int side = 0; side < 2; ++side) {
    if (!valid(skeleton.upper_arm[side]) || !valid(skeleton.forearm[side]) ||
        !valid(skeleton.hand[side])) {
      return false;
    }
  }
  skeleton.below_spine = joints_under(skeleton.parents, skeleton.spine);
  for (int side = 0; side < 2; ++side) {
    skeleton.below_upper_arm[side] = joints_under(skeleton.parents, skeleton.upper_arm[side]);
    skeleton.below_forearm[side] = joints_under(skeleton.parents, skeleton.forearm[side]);
  }
  return true;
}

bool apply(const Skeleton& skeleton, Joint* relative, Joint* absolute, float weight,
           const Rider& rider) {
  if (skeleton.below_spine.empty() || weight <= 0.0f) {
    return false;
  }
  weight = std::min(weight, 1.0f);
  // The line from the lowest spine joint to the neck runs as the rider's does, or leans
  // kLeanDegrees forward of upright.
  float along[3];
  ik::subtract(absolute[skeleton.neck].t, absolute[skeleton.spine].t, along);
  const float lean = kLeanDegrees * kPi / 180.0f;
  float wanted[3] = {0.0f, std::sin(lean), std::cos(lean)};
  // The rider's own lean only while it is one a rider sits in: a rider thrown off or lying down
  // does not bend the passenger over.
  const float spine_length = ik::length(rider.spine);
  if (rider.known && spine_length > 1e-3f &&
      rider.spine[2] / spine_length > std::cos(kMostRiderLean * kPi / 180.0f)) {
    std::copy(rider.spine, rider.spine + 3, wanted);
  }
  float straighten[4];
  ik::rotation_between(along, wanted, straighten);
  float part[4];
  weighted(straighten, weight, part);
  turn_joint(skeleton, relative, absolute, skeleton.spine, part, skeleton.below_spine);
  // The hands at the rider's waist, `weight` of the way from where the clip has them.
  const Joint& hips = absolute[skeleton.hips];
  for (int side = 0; side < 2; ++side) {
    const float sign = side == 0 ? -1.0f : 1.0f;
    float hold[3] = {hips.t[0] + sign * kHandSide, hips.t[1] + kHandAhead, hips.t[2] + kHandUp};
    // The rider's side only within the arm's reach: out of it (the rider not where this body
    // sits, after a fall) the arm stretched out straight after it.
    if (rider.known && within_reach(skeleton, absolute, side, rider.hands[side])) {
      std::copy(rider.hands[side], rider.hands[side] + 3, hold);
    }
    const float* hand = absolute[skeleton.hand[side]].t;
    const float target[3] = {hand[0] + (hold[0] - hand[0]) * weight,
                             hand[1] + (hold[1] - hand[1]) * weight,
                             hand[2] + (hold[2] - hand[2]) * weight};
    reach(skeleton, relative, absolute, side, target);
  }
  return true;
}

}  // namespace kcds::runtime::riding::passenger_hold
