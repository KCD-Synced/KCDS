// A character skin (.skin) or skeleton (.chr) of the game read for the refit of men's pieces to a
// woman's body.
//
// The file is a chunk file (CrCh, version 0x746): a 16-byte header (signature, version, chunk
// count, chunk table offset) and a table of 16-byte entries (type u16, version u16, id, size,
// offset). What the refit reads:
//   CompiledBones (0x2000)  a 32-byte head, then one 584-byte record per joint: the joint's name
//                           (256 bytes at + 312), its parent as an offset in records (+ 572, 0
//                           for the root) and its bind pose in model space as a 3x4 row-major
//                           matrix (+ 264)
//   DataStream (0x1016)     a 24-byte head (flags, stream type, element count, element size, two
//                           reserved words) and the elements: type 0 positions (3 floats), type 9
//                           the bone map (4 joint numbers u16 into the file's own joints, then 4
//                           weights u8 out of 255)
// A refitted piece is the same file with only its positions replaced, so it keeps every other
// chunk byte for byte.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kcds::runtime::appearance::refit {

// A 3x4 row-major transform: rotation in columns 0..2, translation in column 3.
using Matrix34 = std::array<float, 12>;

struct Joint {
  std::string name;
  // The parent's number in the same list; -1 for the root.
  int parent = -1;
  Matrix34 bind{};
};

struct Influence {
  std::array<std::uint16_t, 4> joints{};
  std::array<float, 4> weights{};
};

struct SkinFile {
  std::vector<Joint> joints;
  // x y z of each vertex in the model's space.
  std::vector<float> positions;
  // x y z of each vertex's normal (stream type 1, 3 floats), when the file has them.
  std::vector<float> normals;
  // One per vertex; empty in a skeleton (.chr) that has no mesh.
  std::vector<Influence> influences;
  // Where the positions' elements start in the file, for writing a refitted copy.
  std::size_t positions_offset = 0;
  // Where the bone map's elements start, and the CompiledBones records.
  std::size_t bone_map_offset = 0;
  std::size_t joints_offset = 0;
  // The internal skin vertices (CompiledIntSkinVertices, 0x2005: a 32-byte head, then 64 bytes a
  // vertex, its 4 joint numbers u16 at + 36 and weights float at + 44): where they start, how many.
  std::size_t internal_offset = 0;
  std::size_t internal_count = 0;
};

// The joints and, when it has a mesh, the positions and bone map of a skin or skeleton. False when
// it is not such a file, its joints cannot be read, or a mesh has its positions but no matching
// bone map (or the other way round).
bool read_skin(const std::vector<std::uint8_t>& bytes, SkinFile& skin);

// `bytes` (the file `skin` was read from) with its positions replaced by `positions`; empty when
// the count differs from the file's.
std::vector<std::uint8_t> with_positions(const std::vector<std::uint8_t>& bytes,
                                         const SkinFile& skin, const std::vector<float>& positions);

}  // namespace kcds::runtime::appearance::refit
