// Where the studio's prop is held (Asset/Animation Studio): in a joint of the
// character (the right hand at first, the left hand, or any joint by name) or free in the scene at
// a place and turn. The rope's start and the whip's handle follow it (studio_rope, studio_whip);
// the prop gizmo (studio_prop_gizmo) draws there.
//
//   { op: 'studioPropHold', joint: 'RightHand' | 'LeftHand' | <joint> | '',
//     at: [x, y, z], turn: [x, y, z] }
//
// joint '' is free: the prop stands at `at` (metres), turned `turn` degrees about the world's
// x, then y, then z (the turn z * y * x). In a joint, at and turn are not used.
//
// handle_runtime_message: any thread; the rest: engine thread.
#pragma once

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::studio::hold {

bool handle_runtime_message(const kcd2s::sdk::Value& message);

// The prop's place and turn (x, y, z, w) now: the joint's pose on the character of
// `local_entity`, or the free place. False when the joint is not found.
bool where(void* local_entity, float place[3], float turn[4]);

// Whether the prop is held in a joint (false: free in the scene).
bool in_joint();

// Whether the left hand holds it.
bool in_left_hand();

// Every frame: the character's hand that holds the studio's prop (`prop`: one is made) closes
// round it (pose_animation/hand_grip.hpp); free in the scene, or without a prop, both are open.
void grip(void* local_entity, bool prop);

// Degrees about x, then y, then z as a turn (x, y, z, w), and back.
void turn_of_degrees(const float degrees[3], float turn[4]);
void degrees_of_turn(const float turn[4], float degrees[3]);

}  // namespace kcds::runtime::studio::hold
