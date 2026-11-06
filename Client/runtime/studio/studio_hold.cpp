#include "studio_hold.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>

#include "../pose_animation/hand_grip.hpp"
#include "../props/prop_entity.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "studio_view.hpp"

namespace kcds::runtime::studio::hold {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

constexpr float kDegrees = 3.14159265358979f / 180.0f;
constexpr std::size_t kLongestJoint = 64;

struct Hold {
  // The joint's name; empty: free in the scene.
  std::string joint = "RightHand";
  float at[3] = {32.6f, 31.4f, 33.3f};
  float turn[4] = {0, 0, 0, 1};
};

std::mutex hold_mutex;
Hold held;

}  // namespace

void turn_of_degrees(const float degrees[3], float turn[4]) {
  const float hx = degrees[0] * kDegrees * 0.5f;
  const float hy = degrees[1] * kDegrees * 0.5f;
  const float hz = degrees[2] * kDegrees * 0.5f;
  const float cx = std::cos(hx);
  const float sx = std::sin(hx);
  const float cy = std::cos(hy);
  const float sy = std::sin(hy);
  const float cz = std::cos(hz);
  const float sz = std::sin(hz);
  // z * y * x.
  turn[0] = sx * cy * cz - cx * sy * sz;
  turn[1] = cx * sy * cz + sx * cy * sz;
  turn[2] = cx * cy * sz - sx * sy * cz;
  turn[3] = cx * cy * cz + sx * sy * sz;
}

void degrees_of_turn(const float turn[4], float degrees[3]) {
  const float x = turn[0];
  const float y = turn[1];
  const float z = turn[2];
  const float w = turn[3];
  const float sin_y = std::clamp(2.0f * (w * y - z * x), -1.0f, 1.0f);
  degrees[0] = std::atan2(2.0f * (w * x + y * z), 1.0f - 2.0f * (x * x + y * y)) / kDegrees;
  degrees[1] = std::asin(sin_y) / kDegrees;
  degrees[2] = std::atan2(2.0f * (w * z + x * y), 1.0f - 2.0f * (y * y + z * z)) / kDegrees;
}

bool handle_runtime_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "studioPropHold") {
    return false;
  }
  Hold next;
  const sdk::Value* joint = integration::script_field(message, "joint");
  next.joint = joint && joint->tag() == sdk::Tag::Str ? joint->as_str() : std::string();
  if (next.joint.size() > kLongestJoint) {
    return true;
  }
  double at[3];
  if (view::numbers_of(message, "at", at, 3)) {
    for (int axis = 0; axis < 3; ++axis) {
      next.at[axis] = float(at[axis]);
    }
  }
  double degrees[3];
  if (view::numbers_of(message, "turn", degrees, 3)) {
    const float given[3] = {float(degrees[0]), float(degrees[1]), float(degrees[2])};
    turn_of_degrees(given, next.turn);
  }
  std::lock_guard<std::mutex> lock(hold_mutex);
  held = next;
  return true;
}

bool where(void* local_entity, float place[3], float turn[4]) {
  Hold now;
  {
    std::lock_guard<std::mutex> lock(hold_mutex);
    now = held;
  }
  if (!now.joint.empty()) {
    // A hand holds it in its palm: the game's own grip joint there (RightHandSlot, where it puts
    // weapons, 7 cm from the wrist; the hand's own axes), not the wrist.
    const std::string joint =
        now.joint == "RightHand" || now.joint == "LeftHand" ? now.joint + "Slot" : now.joint;
    return local_entity && props::entity::joint_pose(local_entity, joint.c_str(), place, turn);
  }
  std::copy(std::begin(now.at), std::end(now.at), place);
  std::copy(std::begin(now.turn), std::end(now.turn), turn);
  return true;
}

void grip(void* local_entity, bool prop) {
  std::string joint;
  {
    std::lock_guard<std::mutex> lock(hold_mutex);
    joint = held.joint;
  }
  const bool in_hand = prop && (joint == "RightHand" || joint == "LeftHand");
  pose_animation::hand_grip::hold(local_entity, in_hand, joint == "LeftHand");
}

bool in_joint() {
  std::lock_guard<std::mutex> lock(hold_mutex);
  return !held.joint.empty();
}

bool in_left_hand() {
  std::lock_guard<std::mutex> lock(hold_mutex);
  return held.joint == "LeftHand";
}

}  // namespace kcds::runtime::studio::hold
