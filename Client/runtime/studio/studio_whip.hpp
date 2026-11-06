// The studio's whip in the local player's hand: a physical_props::WhipProp (the
// game's whip handle with a lash of the game's rope physics) put every frame where the right
// hand is and turned with it (joint_pose).
//
// The swing test: the hand's place goes round a circle fast and the handle points out of it, so
// the lash cracks round like a whip.
//
// Engine thread, except handle_runtime_message.
#pragma once

#include <vector>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::studio::whip {

// The studio's editor: { op: 'studioWhip', segments, length } (the lash's), { op:
// 'studioWhipSweep', on } and { op: 'studioWhipOff' }, carried out at the next tick, and { op:
// 'studioLashShape', points } (the animation's lash keys at the playhead: points from the
// handle's tip in its frame, along, right, up; [] for physics). False for any other message.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Every frame in the studio's world, before the rope's tick: the handle in the hand, the lash at
// its tip.
void tick(void* local_entity);

// The whip's lash points in the world (start first), empty while there is no whip: the rope's
// recording into keys (studio_rope.hpp) records a whip's lash with them.
std::vector<float> lash_points();

// The handle's long side and up side in the world (WhipProp::handle_frame), recorded with the
// lash so the editor can make lash keys of it (Lash to keys); false without a whip.
bool handle_frame(float along[3], float up[3]);

// The character's forward and up in the world, as the whip's last tick saw it: recorded with the
// lash so the editor can keep a lash in the body's frame (Lash from motion); false before.
bool body_frame(float forward[3], float up[3]);

// Whether the studio has its whip now.
bool made();

}  // namespace kcds::runtime::studio::whip
