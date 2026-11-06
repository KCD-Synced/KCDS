// The studio's rope in the local player's hand: a physical_props::RopeProp (the
// game's own rope, see rope_prop.hpp for the natives) whose start follows the right hand every
// frame (the hand bone's place); the rest hangs and swings by the game's physics, so a fast hand
// makes a whip. The swing test puts the start on a fast circle instead.
//
// Engine thread.
#pragma once

#include <functional>
#include <string>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::studio::rope {

// "rope <segments> <length in metres>": a rope in the right hand (the old one goes). "rope off":
// takes it away. Lines of the studio's test hook.
void command(const std::string& line);

// The studio's editor (Client/resources/studio/editor.cjs), any thread: { op: 'studioRope',
// segments, length }, { op: 'studioRopeSweep', on }, { op: 'studioRopeEnd', on, at } (its end
// tied to a world point or free) and { op: 'studioRopeOff' }, carried out on
// the engine thread at the next tick. Recording into keys:
//   { op: 'studioRopeRecord', seconds }     the simulation's points 30 times a second (0.5 to
//                                            10 s, physical_props::RopeProp::points), answered
//                                            { op: 'studioRopeRecorded', rate, frames: [[x, y,
//                                            z, ...], ...], handles? } (world metres, start
//                                            first; a whip's handle frame with each key, its long
//                                            side and up side [ax, ay, az, ux, uy, uz])
//   { op: 'studioRopePlay', rate, frames }  those keys in place of the simulation, looped
//                                            (RopeProp::set_points); no frames: simulates again
// False for any other message.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Every frame in the studio's world: keeps the rope's start in the hand, records and plays keys.
void tick(void* local_entity, const std::function<bool(const kcd2s::sdk::Value&)>& send);

// Whether the studio has its rope (or chain) now.
bool made();

}  // namespace kcds::runtime::studio::rope
