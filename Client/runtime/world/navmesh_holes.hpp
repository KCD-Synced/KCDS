// Holes in the game's navmesh where a client script says nobody walks (Terrain.blockNavmesh; a
// World Architect house on a game map): the polygons in a box get the walk flags (1, 2 and 4) taken
// away, so the game's NPCs plan their paths around them, and get them back when it is lifted.
//
//   { op: 'localNavmesh', kind: 'block', id, min: [x, y, z], max: [x, y, z] }
//   { op: 'localNavmesh', kind: 'unblock', id }
//
// The game's own Detour navmesh (wh::xgenaimodule): its navmeshes are the ones its nearest-polygon
// queries use (seen by a hook); a box's polygons come from its polygon query and their flags are
// read and written with its own functions. Tiles stream in and out, so a block is laid again every
// few seconds on the tiles there now. Requests: any thread. Orders: engine thread (frame).
#pragma once

#include <cstdint>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::world::navmesh_holes {

using Logger = void (*)(const char* format, ...);

// Finds the game's navmesh functions and hooks its nearest-polygon query. False when any is
// missing: blocks are refused.
bool install(Logger log);

// True when the message was a localNavmesh order (it is queued).
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// A block of the game client's own (a doorway a script keeps locked), queued as a script's is:
// `id` from kOwnBlocks up, so it never meets a script's; the box's centre and half sizes.
constexpr std::uint32_t kOwnBlocks = 0x80000000u;
void block(std::uint32_t id, const float center[3], const float half[3]);
void unblock(std::uint32_t id);

// Lays and lifts what was ordered, and lays the blocks again on tiles that came in.
void frame(bool in_world);

// The world went: the blocks and the navmeshes known go with it.
void reset();

}  // namespace kcds::runtime::world::navmesh_holes
