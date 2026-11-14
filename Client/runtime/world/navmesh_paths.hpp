// Paths on the game's navmesh for scripts: whether a place can be walked to from another, and the
// way as points (WHGame 1.5.5, wh::xgenaimodule's Detour).
//
// The game plans its NPCs' paths with jobs that run a Detour query over several frames: a query
// object (184 bytes; its pool holds 15, built by its constructor) is set up on the navmesh with a
// node pool (init), the start and the goal are put on their nearest polygons, then the sliced A*
// runs (initSlicedFindPath, updateSlicedFindPath until it is no longer in progress,
// finalizeSlicedFindPath: the polygons from the start, and the point it ended on) and the
// polygons become points (findStraightPath). A path to a goal it cannot reach ends on the polygon
// nearest to it. Warhorse's search asks the agent it plans for (the query's + 128, an entity id)
// whether each step is at the goal and fails without one: KCDS's searches name an id of their own,
// which the game's agent lookup answers with KCDS's goal check (a hook). KCDS keeps a query
// object of its own (the game's pool is shared with its AI) and runs the same steps at once, on
// the engine thread, on the first of the navmeshes the game's AI asks of that has both ends
// (navmesh_holes.hpp).
//
// Engine thread.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::world::navmesh_paths {

using Logger = void (*)(const char* format, ...);

// Finds the game's query functions; false when any is missing (paths are then unknown).
bool install(Logger log);

struct Path {
  enum class Status { Found, NoNavmesh, StartOff, GoalOff, Failed };
  Status status = Status::Failed;
  // Whether the path ends at the goal (else it ends where the navmesh comes nearest to it).
  bool reaches = false;
  // The way from the start to its end: x, y, z each, at most kMostPoints.
  std::vector<float> points;
};

// The most points a path gives, and how far (metres each way, up and down twice that) a start or
// goal may be from the navmesh.
constexpr std::size_t kMostPoints = 256;
constexpr float kNearReach = 3.0f;

// The way from `from` to `to`.
Path find(const float from[3], const float to[3]);

// A client script's question (Client/resources/terrain.cjs), a syncRequest: any thread; it is
// answered on the engine thread (frame) and the reply sent with pump.
//   { op: 'localNavmeshPath', from: {x,y,z}, to: {x,y,z} } -> { status, reaches, points }
//     (points: [[x, y, z]])
//   { op: 'localNavmeshNearest', at: {x,y,z}, reach } -> [x, y, z] or null
bool handle_runtime_message(const kcd2s::sdk::Value& message);
void frame(bool in_world);
void pump(const std::function<bool(const kcd2s::sdk::Value&)>& send);

// The world went: the query is set up again on the next world's navmesh.
void reset();

}  // namespace kcds::runtime::world::navmesh_paths
