// World Architect's nature brush: the level's own plants under the brush, which the
// editor then hides with local level edits (local_level_edits.hpp). Asked through the sync channel
// (Client/resources/sync_channel.cjs), answered on the next engine frame:
//
//   { op: 'localLevelPlants', center: { x, y, z }, radius, most }
//     -> [{ mesh, p: [x, y, z] }]   the shown vegetation instances within `radius` metres
//                                   (at most `most`, 256 at most); null outside a world
//   { op: 'localLevelBrushes', center: { x, y, z }, radius, most, all, hidden }
//     -> [{ mesh, p: [x, y, z], turn, scale, hidden }]
//                                   the level's shown loose brushes (stones, logs, crates; not
//                                   buildings, walls, roads or bridges) standing within `radius`
//                                   metres, where they stand now: what follows the ground when
//                                   the Terrain brushes change it; with all, buildings and walls
//                                   too; with hidden, also those their layer hides (the level's
//                                   other story states), marked hidden
//
// Local mode only (local_mode.hpp). handle_runtime_message: any thread; frame and pump: engine
// thread and network thread, as prefabs/local_prefabs.hpp's.
#pragma once

#include <functional>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::architect::level_plants {

// True when the message was a localLevelPlants request (queued).
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Engine thread, every frame: answers the queued requests.
void frame(bool in_world);

// Network thread: sends the answers made.
void pump(const std::function<bool(const kcd2s::sdk::Value&)>& send);

}  // namespace kcds::runtime::architect::level_plants
