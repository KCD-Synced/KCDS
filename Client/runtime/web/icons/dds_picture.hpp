// A .dds texture's first picture as 8-bit RGBA, for pages that cannot show .dds (web views).
//
// Reads the formats item and buff icons come in: BC7 (the game's own icons), BC1 and BC3 (DXT1,
// DXT5) and uncompressed 32-bit RGBA or BGRA, with or without the DX10 header. Only the top mip
// level is decoded. Pure code: no game or Windows calls.
#pragma once

#include <cstdint>
#include <vector>

namespace kcds::runtime::web::icons {

struct Picture {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgba;  // width * height * 4 bytes, rows from the top
};

// Pictures larger than this on a side are refused (an icon is 64 or 128 pixels).
inline constexpr std::uint32_t kMaxPictureSide = 4096;

// False for a file that is not a .dds of a known format, or is cut short.
bool decode_dds(const std::vector<std::uint8_t>& file, Picture& out);

// One 16-byte BC7 block into 16 RGBA pixels (row by row). Exposed for the tests.
void decode_bc7_block(const std::uint8_t* block, std::uint8_t* pixels);

}  // namespace kcds::runtime::web::icons
