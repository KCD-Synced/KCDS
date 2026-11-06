#include "hand_grip.hpp"

#include <array>
#include <map>
#include <unordered_map>
#include <vector>

#include "../props/prop_entity.hpp"
#include "joint_turns.hpp"
#include "kcd2s/integration/pose_animation.hpp"

namespace kcds::runtime::pose_animation::hand_grip {
namespace {

namespace entity = props::entity;
namespace integration = kcd2s::integration;

struct Finger {
  const char* joint;
  std::array<float, 3> degrees;
};

// The fist (hand_grip.hpp: how these were made).
constexpr Finger kRightFist[] = {
    {"RightHandThumb1", {7.7f, -9.9f, -16.3f}},     {"RightHandThumb2", {-13.8f, -32.5f, 3.3f}},
    {"RightHandThumb3", {-0.7f, -30.0f, -0.3f}},    {"RightHandIndex1", {13.2f, -33.7f, -59.1f}},
    {"RightHandIndex2", {19.2f, -43.8f, -80.7f}},   {"RightHandIndex3", {0.7f, -39.0f, -39.8f}},
    {"RightHandMiddle1", {8.1f, -35.8f, -57.3f}},   {"RightHandMiddle2", {6.3f, -45.8f, -76.1f}},
    {"RightHandMiddle3", {-10.1f, -47.3f, -23.0f}}, {"RightHandRing1", {7.7f, -8.6f, -64.7f}},
    {"RightHandRing2", {-4.4f, -25.1f, -80.8f}},    {"RightHandRing3", {-11.8f, -41.5f, -30.7f}},
    {"RightHandPinky1", {15.2f, 3.4f, -62.8f}},     {"RightHandPinky2", {-14.9f, -7.4f, -82.7f}},
    {"RightHandPinky3", {-41.6f, -39.7f, 11.7f}},
};
constexpr Finger kLeftFist[] = {
    {"LeftHandThumb1", {8.1f, -8.2f, -17.0f}},    {"LeftHandThumb2", {-12.9f, -32.5f, -0.8f}},
    {"LeftHandThumb3", {0.0f, -29.8f, -3.8f}},    {"LeftHandIndex1", {11.4f, -28.0f, -61.2f}},
    {"LeftHandIndex2", {14.6f, -36.8f, -81.9f}},  {"LeftHandIndex3", {1.8f, -32.9f, -45.3f}},
    {"LeftHandMiddle1", {8.1f, -29.5f, -60.3f}},  {"LeftHandMiddle2", {3.6f, -35.9f, -79.5f}},
    {"LeftHandMiddle3", {-2.3f, -39.9f, -37.7f}}, {"LeftHandRing1", {7.0f, -1.6f, -64.7f}},
    {"LeftHandRing2", {-5.7f, -9.7f, -83.9f}},    {"LeftHandRing3", {-5.5f, -28.6f, -45.8f}},
    {"LeftHandPinky1", {14.9f, 10.5f, -61.3f}},   {"LeftHandPinky2", {-17.4f, 5.5f, -84.2f}},
    {"LeftHandPinky3", {-42.2f, -39.2f, 12.6f}},
};

// What each body's hands do now: which hand, and its character (to set and forget the grip).
struct Grip {
  void* character = nullptr;
  int hand = -1;  // -1 open, 0 right, 1 left
  // The skeleton's joint count when the fist was made (its ids belong to that skeleton).
  int joints = 0;
};
std::unordered_map<void*, Grip> grips;

std::vector<joint_turns::Turn> fist(void* body, bool left_hand) {
  std::vector<joint_turns::Turn> turns;
  const auto add = [&](const Finger& finger) {
    const int id = entity::joint_id(body, finger.joint);
    if (id >= 0) {
      joint_turns::Turn turn;
      turn.joint = id;
      turn.rotation = integration::pose_quaternion(finger.degrees);
      turns.push_back(turn);
    }
  };
  if (left_hand) {
    for (const Finger& finger : kLeftFist) {
      add(finger);
    }
  } else {
    for (const Finger& finger : kRightFist) {
      add(finger);
    }
  }
  return turns;
}

}  // namespace

void hold(void* body, bool holding, bool left_hand) {
  void* character = body ? entity::character(body) : nullptr;
  if (!character) {
    forget(body);
    return;
  }
  const int hand = holding ? (left_hand ? 1 : 0) : -1;
  // The joints' ids change with the body's skeleton (the studio's clothes make it 666 joints from
  // 493, live: the thumb 191, then 366), so the fist is made again when its joint count changes.
  const int joints = entity::joint_count(body);
  Grip& grip = grips[body];
  if (grip.character == character && grip.hand == hand && grip.joints == joints) {
    return;
  }
  grip.joints = joints;
  if (grip.character && grip.character != character) {
    joint_turns::forget_grip(grip.character);
  }
  grip.character = character;
  grip.hand = hand;
  if (hand < 0) {
    joint_turns::forget_grip(character);
  } else {
    joint_turns::set_grip(character, fist(body, hand == 1));
  }
}

void forget(void* body) {
  const auto found = grips.find(body);
  if (found == grips.end()) {
    return;
  }
  if (found->second.character) {
    joint_turns::forget_grip(found->second.character);
  }
  grips.erase(found);
}

}  // namespace kcds::runtime::pose_animation::hand_grip
