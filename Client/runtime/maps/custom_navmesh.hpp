// A custom map's navmesh, made in the game client from the ground as it is now (World Architect's
// Bake navmesh): the same as Dev/tools/architect/make_custom_map.py writes, Warhorse's Detour
// version 14 as the game reads it (index, tiles, add):
//
//   Recast/RDNavmesh_1.hdr   records of int tile x, tile y, offset into the .nav, 1, tile size
//   Recast/RDNavmesh_1.nav   the navigation settings (Scripts/AI/Navigation.xml's, 56 bytes), the
//                            tile count, then the tiles: a 112-byte header, float x y z vertices,
//                            28-byte polygons (6 vertex indices, 6 neighbours, flags, vertex count)
//
// 16 m tiles of 4 x 4 walkable squares at the ground's heights; a square under a blocked area
// (a building, a prop) is left out, its neighbours walled there.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace kcds::runtime::maps::custom {

// An area people cannot walk through, on the ground plane, metres.
struct Blocked {
  float min_x = 0;
  float min_y = 0;
  float max_x = 0;
  float max_y = 0;
};

// The ground's height at a point; false where there is no ground.
using HeightAt = std::function<bool(float x, float y, float& height)>;

struct NavmeshFiles {
  std::vector<std::uint8_t> index;
  std::vector<std::uint8_t> tiles;
  int tile_count = 0;
  int walkable = 0;
  int left_out = 0;
};

// The navmesh of a square map `size` metres a side.
NavmeshFiles make_navmesh(int size, const HeightAt& height_at, const std::vector<Blocked>& blocked);

}  // namespace kcds::runtime::maps::custom
