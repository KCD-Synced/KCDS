// The level's grass carpet while World Architect shapes the ground: it moves with the
// ground, and the nature brush can hide it.
//
// WHGame 1.5.5 draws a vegetation group the level marks for automatic merging (byte + 314 of its
// 416-byte group) as no render node of its own: CTerrain::AddVegetationInstance gives
// it to CMergedMeshesManager::AddInstance, which keeps it in the merged-mesh render
// node (CMergedMeshRenderNode, render node type 23) of its 16 m cell. Such a node
// holds:
//
//   + 140   the cell's origin (x, y, z)
//   + 240   its group headers, 128 bytes each, their count at + 248; a header's instances at + 0,
//           their count at + 84; an instance is 16 bytes: its place as three u16
//           ((place - origin) / 16 m * 65535), its turn (+ 6 .. + 9) and its size (+ 12, + 14)
//   + 108   set to 1 and + 112 counted up after a change, as AddInstance does
//
// The nodes in a box are gathered as the game's crater gathers roads (with the node
// type). The cells stream out and in again, so hidden grass is hidden again every 2 seconds.
//
// before() and after() bracket a ground edit: the grass standing on the ground in the box is moved
// by as much as the ground under it moved. Engine thread.
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace kcds::runtime::terrain::grass {

using Logger = void (*)(const char* format, ...);

// False when the game's functions were not found (the grass then stays as the level has it).
bool install(Logger logger);

// Before a ground edit in the box (min x, y, z, max x, y, z): the grass on the ground there and
// the ground's height under each.
void before(const float box[6]);

// After that edit: that grass moved with the ground. Returns how many instances moved.
int after();

// Hides the grass within `radius` metres of (x, y) as `id`; show(id) brings it back.
int hide(std::uint32_t id, float x, float y, float radius);
int show(std::uint32_t id);

// The kinds of the level's grass carpet within `radius` metres of (x, y): { vegetation kind,
// how many instances of it }, for the nature brush to plant the grass the ground has. Engine
// thread.
std::vector<std::pair<int, int>> kinds_near(float x, float y, float radius);

// Every frame in a world: hidden grass hidden again in cells that came back.
void frame();

// The world went: nothing is kept.
void reset();

}  // namespace kcds::runtime::terrain::grass
