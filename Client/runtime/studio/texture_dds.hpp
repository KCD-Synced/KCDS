// A picture as a .dds the game's renderer loads (Asset/Animation Studio): 32-bit
// BGRA without compression (DDPF_RGB | DDPF_ALPHAPIXELS, masks R 0x00FF0000, G 0x0000FF00,
// B 0x000000FF, A 0xFF000000), with every mip level down to 1 x 1, each halved with a 2 x 2 box
// filter. The game's own textures are compressed; an uncompressed one costs four bytes a pixel
// and needs no tool.
#pragma once

#include <cstdint>
#include <vector>

namespace kcds::runtime::studio::texture {

struct Image {
  int width = 0;
  int height = 0;
  // Rows from the top, four bytes a pixel: blue, green, red, alpha.
  std::vector<std::uint8_t> bgra;
};

// The next mip level: each side halved (at least 1), each pixel the mean of the 2 x 2 (or fewer,
// at an odd edge) it covers.
Image half_of(const Image& image);

// How many mip levels a picture of this size has (1 x 1 included).
int mip_count(int width, int height);

// The .dds file's bytes; empty for an image without pixels. `srgb`: the colours are sRGB (a
// picture's), so the game reads them as such (CryEngine's image flags: the marker
// 'CRYF' at byte 124, the flags at byte 36 with EIF_SRGBRead 0x800, then an empty CExt ... CEnd
// chunk after the pixels); false for a normal map.
std::vector<std::uint8_t> dds_of(const Image& image, bool srgb = false);

}  // namespace kcds::runtime::studio::texture
