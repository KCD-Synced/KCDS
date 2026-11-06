// World Architect's door and gate preview,
// in this game only. Local mode
// only; nothing is replicated, and the level's door states are the game's own after the next load.
//
// Doors open and close by the game's own OpenDoor and CloseDoor (doors/doors.hpp). Gates (the
// animated drawbridge and portcullis, and the levels' gate series) move as a server moves them
// (gates/gates.hpp): their module runs here with the states the Architect sets, from the level's
// own state until one is set.
//
//   { op: 'localDoor', guid, open }      opens (true) or closes (false) the door of that guid
//   sync localDoorState { guid }          { door, open, locked }, door false for no door
//   { op: 'localGate', gate, state }      moves a gate (its number) into a state (its index)
//   sync localGatesNear { at, radius }    [{ gate, key, state, states }] of this map's gates within
//                                         radius metres (at most 60) of at; state -1: the level's
//
// guid: the entity's EntityGuid as sixteen hex digits. Requests: any thread
// (handle_runtime_message). frame(): engine thread. pump(): network thread.
#pragma once

#include <functional>
#include <vector>

#include "kcd2s/integration/level_edits.hpp"
#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::architect::level_doors {

// True when the message was one of the above.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Opens and closes what was asked, moves the gates and answers the questions, in a ready world;
// the gates' level edits (their static models) are added to `level_edits`.
void frame(bool in_world, std::vector<kcd2s::integration::LevelEditChange>& level_edits);

// The world is left: the gates' states set here are forgotten.
void reset();

// Sends the answers made.
void pump(const std::function<bool(const kcd2s::sdk::Value&)>& send);

}  // namespace kcds::runtime::architect::level_doors
