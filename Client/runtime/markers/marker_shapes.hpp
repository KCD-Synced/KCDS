// The mesh shapes of a world marker: cylinder, sphere, chevron, cube and plane, drawn
// every frame with the renderer's aux geometry (gizmos.hpp solid) as faces in the marker's colour
// and edges a little brighter. The game has no plain meshes for these, and the aux geometry also
// gives the colour, the spin and the bob each frame without a body in the world.
//
// The shapes are built around the marker's position, as the server's trigger volumes are
// (Backend/scripting/runtimes/js/markers/markers.cjs holds): a sphere is a ball of diameter `size`
// about it; a cylinder and a cube stand `size` wide on it, `height` tall; a plane lies flat on it,
// `size` square; a chevron points down at it from `height` above.
//
// Engine thread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kcds::runtime::markers::shapes {

struct Mesh {
  std::vector<float> points;  // x, y, z in a row, about the marker's position
  std::vector<std::uint32_t> triangles;
  std::vector<std::uint32_t> lines;
};

// The mesh of a shape name; empty for "decal" or a name it does not know.
Mesh build(const std::string& shape, float size, float height);

}  // namespace kcds::runtime::markers::shapes
