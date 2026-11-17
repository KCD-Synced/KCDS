// How a horse's passenger sits: upright, holding on to the rider (horses/passengers.cjs).
//
// The passenger plays the rider's own sitting clip on a horse (MotionIdle horse+player), which
// leans forward over the reins. The game has no clip of someone riding behind a rider, so the pose
// is changed after the clip, on every game: the spine is turned upright (kLeanDegrees forward of
// straight up, from the lowest spine joint), and each hand reaches to the rider's side at the
// waist, where a passenger holds on, with a two-bone reach (arm_ik.hpp): the elbow bends to the
// distance, then the upper arm turns the hand onto the place. The rider sits right ahead
// (passengers.cjs BEHIND_RIDER, ABOVE_RIDER), so the places are fixed in the passenger's own space,
// measured from their hips.
//
// apply() works on a pose's joint arrays (relative to the parent and in the character's space, as
// the game's pose modifiers read them); passenger_pose.hpp runs it in the game's animation pass.
#pragma once

#include <cstdint>
#include <vector>

namespace kcds::runtime::riding::passenger_hold {

// A joint's rotation (x, y, z, w) and place, as the engine's QuatT.
struct Joint {
  float q[4];
  float t[3];
};
static_assert(sizeof(Joint) == 28, "the engine's QuatT is 28 bytes");

inline constexpr std::uint16_t kNoParent = 0xFFFF;

// How far forward of straight up the spine stays (degrees), and where the hands hold on, from the
// hips: metres to each side, ahead and up (the rider's waist, the character's space: x right, y
// forward, z up).
inline constexpr float kLeanDegrees = 6.0f;
inline constexpr float kHandSide = 0.18f;
inline constexpr float kHandAhead = 0.36f;
inline constexpr float kHandUp = 0.08f;

// The joints apply() turns, by their index in the pose, and everything under each (parents first).
struct Skeleton {
  int hips = -1;
  int spine = -1;
  int neck = -1;
  int upper_arm[2] = {-1, -1};  // left, right
  int forearm[2] = {-1, -1};
  int hand[2] = {-1, -1};
  std::vector<std::uint16_t> parents;
  std::vector<int> below_spine;
  std::vector<int> below_upper_arm[2];
  std::vector<int> below_forearm[2];
};

// The joints under `joint`, nearest first, so each parent comes before its children.
std::vector<int> joints_under(const std::vector<std::uint16_t>& parents, int joint);

// Fills the lists under the named joints once their indices and the parents are set. False when a
// joint is missing.
bool prepare(Skeleton& skeleton);

// The rider in front as this game shows them, in the passenger's character space: the way their
// spine runs (from its lowest joint to the neck) and where the passenger's hands hold on to their
// sides (left, right). With it the passenger leans as the rider does, braking, sprinting and in
// turns, and holds on to the rider where the rider is: the rider's clips are the game's riding
// blend driven by the horse, which the passenger's body does not play, and leaning back the rider
// came into the passenger's view. Without it (no rider seen) they sit upright.
struct Rider {
  bool known = false;
  float spine[3] = {};
  float hands[2][3] = {};
};

// Changes the pose: `weight` from 0 (the clip's own) to 1 (leaning with the rider or upright,
// holding on), so the hold comes in smoothly. Both arrays are kept consistent. False when the
// skeleton is not prepared.
bool apply(const Skeleton& skeleton, Joint* relative, Joint* absolute, float weight,
           const Rider& rider = Rider{});

}  // namespace kcds::runtime::riding::passenger_hold
