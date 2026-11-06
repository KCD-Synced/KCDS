// A studio mesh's own vertices and triangles read from its .cgf (Asset/Animation Studio, issue):
// the cloth's physics mesh is made of exactly the vertices the game draws, in the same
// order, so the model's SyncToRenderMesh moves each drawn vertex with its own
// physics vertex.
//
// The file (CrCh, version 0x746, as Dev/tools/assets/blender/kcds_cgf.py writes it; the header
// and chunk table as studio/cgf_patch.hpp describes them): DataStream chunks (type 0x1016) hold
// a stream type, an element count and size, two reserved words and the elements after a 24-byte
// head. Positions are stream 15 (P3S_C4B_T2S, 16 bytes: half x y z w, colour, half u v) and the
// triangles stream 5 (16-bit vertex numbers, three per triangle).
#pragma once

#include <cstdint>
#include <vector>

namespace kcds::runtime::studio::cgf {

struct MeshShape {
  // x y z of each vertex, in the model's own space.
  std::vector<float> positions;
  // Three vertex numbers per triangle.
  std::vector<std::uint16_t> indices;
};

// The model's vertices and triangles; false when it is not such a file, a stream is missing or a
// triangle names a vertex it does not have.
bool read_shape(const std::vector<std::uint8_t>& model, MeshShape& shape);

}  // namespace kcds::runtime::studio::cgf
