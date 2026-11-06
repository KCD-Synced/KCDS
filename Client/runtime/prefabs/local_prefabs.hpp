// The game's prefabs a client script builds in this game only (the client Prefab,
// Client/resources/prefabs.cjs; World Architect): what an editor places while its author
// works. Nothing is replicated.
//
//   { op: 'localPrefab', kind: 'spawn' | 'move', id, template, position, rotation }
//   { op: 'localPrefab', kind: 'destroy', id }
//
// Built as kcds_local_prefab_<id> with game_prefabs.hpp; a move builds it again at the new place.
//
// World Architect's previews ask through the sync channel ({ op: 'syncRequest', id, request }):
//   { op: 'localPrefabPieces', template } -> [{ mesh, offset: [x, y, z], rotation: [x, y, z, w] }]
// the template's brushes (game_prefabs.hpp pieces), answered on the engine thread (frame) and
// sent by the network thread (pump).
// Requests: any thread (handle_runtime_message). Orders: engine thread (frame).
#pragma once

#include <functional>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::prefabs::local {

// True when the message was a localPrefab order (it is queued).
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Builds, moves and takes away what was ordered since the last frame; in a ready world only.
void frame(bool in_world);

// The world went (a level change, leaving): its prefabs went with it.
void reset();

// Sends the answers to the pieces requests; network thread.
void pump(const std::function<bool(const kcd2s::sdk::Value&)>& send);

}  // namespace kcds::runtime::prefabs::local
