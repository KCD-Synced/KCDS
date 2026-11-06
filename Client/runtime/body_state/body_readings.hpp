// How the players' bodies stand and move on this machine.
//
// Posture, read from the local player's own game and published in the NetworkPlayer field
// "posture" (kcd2s/integration/posture.hpp):
// - In the air: the body's living physics. Its entity's physical entity (the entity's vtable
//   slot 79) gives its living status (GetStatus, slot 6, status type 2; a living
//   entity's is): flying (+ 4) for longer than a stair step lasts (its time flying,
//   + 8). A body that is not a living one (a ragdoll) is not in the air.
// - Crouched: the owner's own animation stance is its crouch (the Stance tag "stealth").
// - Physics profile: the profile of the actor's game object (+ 40) for its physics aspect
//   (GetAspectProfile(8), its vtable slot 25): 0 unragdoll, 1 alive, 2 ragdoll,
//   3 sleep, 6 spectator, as the actor script binding's GetPhysicalizationProfile names them
//  ; 255 without a game object.
//
// Mannequin tags, read for every player's body on this machine (the local player's and every
// replica's): the actor's animated human (its vtable slot 220) holds its
// CAnimatedCharacter (+ 32), whose action controller (GetActionController, slot 37)
// has the animation context (GetContext, slot 22): the tag definition (+ 8) and the
// tag state (+ 16) the game's animation picked. A tag of a group is the one whose packed value
// the state holds in the group's bits (body_readings.cpp).
//
// Engine thread.
#pragma once

#include <cstdint>

namespace kcds::runtime::body_state {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* animated_human = nullptr;     // the actor's animated human getter
  void* action_controller = nullptr;  // CAnimatedCharacter::GetActionController
  void* context = nullptr;            // the action controller's GetContext
  void* physics = nullptr;            // the entity's GetPhysics
  void* living_status = nullptr;      // a living physical entity's GetStatus
  void* aspect_profile = nullptr;     // the game object's GetAspectProfile
};

// False when a native is missing.
bool install(const Natives& natives, Logger logger);

// A Mannequin tag the body's animation has not set.
inline constexpr std::uint8_t kNoTag = 255;

// The tags this machine's animation system picked for a body: raw tag ids of its own tag
// definition, kNoTag for none; `crouched` when its stance is its tag definition's "stealth".
struct BodyTags {
  std::uint8_t move_speed = kNoTag;
  std::uint8_t move_dir = kNoTag;
  std::uint8_t stance = kNoTag;
  bool crouched = false;

  friend bool operator==(const BodyTags& a, const BodyTags& b) {
    return a.move_speed == b.move_speed && a.move_dir == b.move_dir && a.stance == b.stance &&
           a.crouched == b.crouched;
  }
};

// The tags of an actor's body; none without an animation context. A body that sits (holds a seat)
// while its stance group holds no tag has its tag definition's "sitting" stance: a
// replica sits without the game's own seat use, and the game's use does not always set it.
BodyTags read_tags(void* actor, bool seated);

// The Mannequin action controller of an actor's body; nullptr without one.
void* action_controller(void* actor);

// How a body stands: physics flying (and for how long) and its physics profile.
struct BodyPhysics {
  bool flying = false;
  float time_flying = 0.0f;
  std::uint8_t profile = 255;
};

BodyPhysics read_physics(void* actor);

// Whether a body that has been flying for `time_flying` seconds is in the air rather than
// stepping down a stair.
bool in_air(const BodyPhysics& physics);

}  // namespace kcds::runtime::body_state
