// A hand closed round what it holds (Asset/Animation Studio): the fingers'
// turns of a fist round a whip's handle or a rope, put on the body under its animation
// (joint_turns.hpp set_grip) while a prop is in that hand, in the studio and on the players.
//
// The turns: each finger joint (Thumb, Index, Middle, Ring, Pinky 1 to 3) turned toward the palm
// round the axis across its bone and the palm's facing (the palm faces the thigh: -x of the
// body's right, forward, up frame for the right hand, +x for the left), 65, 85 and 55 degrees
// for the fingers and 20, 35 and 30 for the thumb, made into the joint's parent's x, y, z degrees
// (the studio's turns). Measured from the joints' axes over MCP (studio_joint_info) on the male
// body and checked in a close look at the hand.
//
// Engine thread.
#pragma once

namespace kcds::runtime::pose_animation::hand_grip {

// Closes the body's hand (left or right) round what it holds, the other hand open; `holding`
// false opens both. `body` is the body's entity (its character from it).
void hold(void* body, bool holding, bool left_hand);

// The body's grip goes (it is gone).
void forget(void* body);

}  // namespace kcds::runtime::pose_animation::hand_grip
