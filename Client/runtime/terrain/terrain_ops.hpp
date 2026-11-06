// The client Terrain's orders (Client/resources/terrain.cjs; World Architect terraforming): a
// client script shapes the ground in this game only.
//
//   { op: 'localTerrain', kind: 'edit', id, shape: 'flatten' | 'raise', x, y, radius, falloff,
//     value }
//   { op: 'localTerrain', kind: 'revert', id }
//
// Requests: any thread (handle_runtime_message). Orders: engine thread (frame), applied with
// terrain_edits.hpp.
#pragma once

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::terrain::ops {

// True when the message was a localTerrain order (it is queued).
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Applies what was ordered since the last frame; in a ready world only.
void frame(bool in_world);

// The world went: the orders not applied yet go with it.
void reset();

}  // namespace kcds::runtime::terrain::ops
