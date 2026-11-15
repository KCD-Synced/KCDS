// The physics proxies a game model's file (.cgf, CrCh 0x746) holds, read from its bytes.
//
// The file is a table of chunks (u16 type, u16 version, u32 id, u32 size, u32 offset). A node
// (0x100B) has its name (64 bytes), its mesh's chunk id (+ 64), its parent node's (+ 68) and its
// transform relative to the parent (4x4 floats from + 84, rows, the place in the last row). A
// mesh (0x1000) names its physics data chunk (0x1018) at + 92; that chunk's data, after a 24-byte
// head that starts with its size, is what the game's geometry manager loads a proxy from.
//
// No game calls here, so it is tested on its own.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace kcds::runtime::props::cgf {

// A rigid transform: a 3x3 of rows (the file's row vector convention) and a place.
struct Transform {
  std::array<float, 9> rows{1, 0, 0, 0, 1, 0, 0, 0, 1};
  std::array<float, 3> place{};
};

// One proxy: where its data is in the file, how long it is, and its node's place in the model
// (through its parents).
struct Proxy {
  std::size_t offset = 0;
  std::uint32_t size = 0;
  Transform where;
};

// The proxies of the file; none when it is not a CrCh 0x746 file or holds none.
std::vector<Proxy> proxies_of(const std::vector<std::uint8_t>& file);

// `inner` (relative to `outer`) in `outer`'s space.
Transform combine(const Transform& inner, const Transform& outer);

// The rotation of a row matrix as a unit quaternion (x, y, z, w) that turns column vectors the
// same way (the matrix transposed).
std::array<float, 4> quaternion_of(const std::array<float, 9>& rows);

}  // namespace kcds::runtime::props::cgf
