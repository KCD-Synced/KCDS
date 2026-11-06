// The studio's posed joints: the turns the editor gives joints of the character,
// added every frame on top of its animation (pose_animation/joint_turns.hpp, the game's own joint
// operator queue).
//
//   { op: 'studioPose', joints: [[id, x, y, z, w], ...], lift? }   from the editor: the turns
//        (quaternions); [] for none; lift: the hips raised or lowered (metres, a baked clip's
//        keys)
//
// The body stands still in the studio: its own animation (the idle) is held where it is with the
// character's SetPlaybackScale (slot 41, + 328; what the entity binding SetAnimationSpeed calls
// for no layer) at 0, every frame; the turns still apply on top.
//
//   { op: 'studioIdle', on }   the body's own animation plays again (on) or is held (default)
//
// handle_runtime_message: any thread; tick: engine thread.
#pragma once

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::studio::pose {

bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Every frame in the studio's world: holds the body still (or lets its idle play) and gives the
// turns to its character.
void tick(void* local_entity);

}  // namespace kcds::runtime::studio::pose
