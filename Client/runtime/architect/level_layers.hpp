// World Architect's story layers: the level's layers around a point and switching them
// (level/story_layers.hpp). Asked through the sync channel (Client/resources/sync_channel.cjs),
// answered on the next engine frame:
//
//   { op: 'localLevelLayers', map, center: { x, y, z }, radius, most }
//     -> [{ id, name, short, path, on, brushes, shown, entities, p: [x, y, z] }]
//                                   the layers of `map` with brushes or entities within `radius`
//                                   metres (at most `most`, 64 at most), nearest first; null
//                                   outside a world
//   { op: 'localLevelLayer', name, on }
//     -> true when the game has the layer and it is now as asked, else false
//   { op: 'localLevelLayersRestore' }
//     -> true: every layer switched here is back as the level had it
//
// Local mode only (local_mode.hpp). handle_runtime_message: any thread; frame: engine thread;
// pump: network thread, as level_plants.hpp's.
#pragma once

#include <functional>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::architect::level_layers {

// True when the message was one of these requests (queued).
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Engine thread, every frame: answers the queued requests.
void frame(bool in_world);

// Network thread: sends the answers made.
void pump(const std::function<bool(const kcd2s::sdk::Value&)>& send);

}  // namespace kcds::runtime::architect::level_layers
