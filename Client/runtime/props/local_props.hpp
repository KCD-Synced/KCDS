// Props a client script puts in this game only (World Architect's Props content, the owner
// "in the game is a lot more stuff"): any mesh of the game's prop catalogue (furniture,
// decorations, fixtures, tools, food, rocks...) as a solid static entity. Nothing is replicated.
//
//   { op: 'localProp', kind: 'set', id, mesh, position, rotation, scale (0.1 to 10, default 1),
//     material (a game path without .mtl, drawn in place of the mesh's own; optional) }
//   { op: 'localProp', kind: 'remove', id }
//
// A set of an id that stands with the same mesh moves it (its world matrix, the entity stays); with
// another mesh it is built again. Only a prop's last order of a frame is made (a drag sends many).
//
// Requests: any thread (handle_runtime_message). Orders: engine thread (frame).
#pragma once

#include <functional>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::props::local {

// True when the message was a localProp order (it is queued).
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Builds, moves and takes away what was ordered since the last frame; in a ready world only. Also
// answers the sync localPropBounds { mesh } question: a standing prop's box in its own frame,
// { min, max }, or null.
void frame(bool in_world);

// Sends the answers made; network thread.
void pump(const std::function<bool(const kcd2s::sdk::Value&)>& send);

// The world went (a level change, leaving): its props went with it.
void reset();

}  // namespace kcds::runtime::props::local
