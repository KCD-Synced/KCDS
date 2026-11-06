// World Architect's Bake navmesh (issue list C4): on a custom map, the navmesh made again from the
// ground as it is now (its terrain edits) with the author's buildings and props left out
// (maps/custom_navmesh.hpp), kept for the game's next start (maps/custom_maps.hpp
// keep_next_navmesh). Local mode only.
//
//   sync localNavmeshBake { blocked: [{ minX, minY, maxX, maxY }] }
//        -> { tiles, walkable, leftOut } or null (not a custom map, or not written)
//
// Requests: any thread (handle_runtime_message). frame(): engine thread. pump(): network thread.
#pragma once

#include <functional>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::architect::navmesh_bake {

// True when the message was the request above.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Bakes what was asked, in a ready world.
void frame(bool in_world);

// Sends the answers made.
void pump(const std::function<bool(const kcd2s::sdk::Value&)>& send);

}  // namespace kcds::runtime::architect::navmesh_bake
