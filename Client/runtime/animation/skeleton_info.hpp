// A body's skeleton for client scripts (Animations.skeleton): every joint's name and parent, and
// its rest pose (the skeleton's default pose in the body's space), read from the body's own
// character, so a script that solves joints itself (a hand's reach, a prop on a joint) does not
// have to guess the hierarchy.
//
// Engine thread (the local world's synchronous answers).
#pragma once

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::animation::skeleton_info {

// { names: "a,b,...", parents: "-1,0,...", rest: "x,y,z,w,px,py,pz,..." }: the joints' names and
// parents' indexes (-1 for the root), and each joint's rest pose in the body's space (x right,
// y forward, z up, from its origin), 7 integers a joint: the rotation in ten-thousandths, the
// position in hundredths of a millimetre. Compact, as a message to the runtime holds at most 64 KB.
// Null for an entity without a skeleton.
kcd2s::sdk::Value describe(void* entity);

}  // namespace kcds::runtime::animation::skeleton_info
