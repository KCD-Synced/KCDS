#include "texture_dds.hpp"

#include <algorithm>
#include <cstring>

namespace kcds::runtime::studio::texture {
namespace {

constexpr std::uint32_t kMagic = 0x20534444;  // "DDS "
constexpr std::uint32_t kHeaderSize = 124;
constexpr std::uint32_t kPixelFormatSize = 32;
// DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_MIPMAPCOUNT.
constexpr std::uint32_t kFlags = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | 0x20000;
// DDPF_ALPHAPIXELS | DDPF_RGB.
constexpr std::uint32_t kPixelFlags = 0x1 | 0x40;
// DDSCAPS_COMPLEX | DDSCAPS_TEXTURE | DDSCAPS_MIPMAP.
constexpr std::uint32_t kCaps = 0x8 | 0x1000 | 0x400000;

// CryEngine's image extension: 'CRYF', 'CExt' and 'CEnd' as little-endian words,
// and the image flag EIF_SRGBRead.
constexpr std::uint32_t kCryMarker = 0x46595243;
constexpr std::uint32_t kExtensionStart = 0x74784543;
constexpr std::uint32_t kExtensionEnd = 0x646E4543;
constexpr std::uint32_t kSrgbRead = 0x800;

constexpr std::uint32_t cry_flags_of(bool srgb) {
  return srgb ? kSrgbRead : 0u;
}

void put(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(std::uint8_t(value >> shift));
  }
}

}  // namespace

Image half_of(const Image& image) {
  Image half;
  half.width = std::max(1, image.width / 2);
  half.height = std::max(1, image.height / 2);
  half.bgra.resize(std::size_t(half.width) * std::size_t(half.height) * 4);
  for (int y = 0; y < half.height; ++y) {
    for (int x = 0; x < half.width; ++x) {
      unsigned sums[4] = {0, 0, 0, 0};
      unsigned count = 0;
      for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
          const int sx = x * 2 + dx;
          const int sy = y * 2 + dy;
          if (sx >= image.width || sy >= image.height) {
            continue;
          }
          const std::uint8_t* pixel =
              &image.bgra[(std::size_t(sy) * std::size_t(image.width) + std::size_t(sx)) * 4];
          for (int part = 0; part < 4; ++part) {
            sums[part] += pixel[part];
          }
          ++count;
        }
      }
      std::uint8_t* out =
          &half.bgra[(std::size_t(y) * std::size_t(half.width) + std::size_t(x)) * 4];
      for (int part = 0; part < 4; ++part) {
        out[part] = std::uint8_t((sums[part] + count / 2) / count);
      }
    }
  }
  return half;
}

int mip_count(int width, int height) {
  int count = 1;
  while (width > 1 || height > 1) {
    width = std::max(1, width / 2);
    height = std::max(1, height / 2);
    ++count;
  }
  return count;
}

std::vector<std::uint8_t> dds_of(const Image& image, bool srgb) {
  const std::size_t pixels = std::size_t(image.width) * std::size_t(image.height);
  if (image.width <= 0 || image.height <= 0 || image.bgra.size() != pixels * 4) {
    return {};
  }
  const int mips = mip_count(image.width, image.height);
  std::vector<std::uint8_t> out;
  out.reserve(128 + pixels * 4 * 4 / 3 + 64);
  put(out, kMagic);
  put(out, kHeaderSize);
  put(out, kFlags);
  put(out, std::uint32_t(image.height));
  put(out, std::uint32_t(image.width));
  put(out, std::uint32_t(image.width) * 4);  // the pitch of a row
  put(out, 0);                               // depth
  put(out, std::uint32_t(mips));
  // dwReserved1: [1] (byte 36) the CryEngine image flags.
  for (int reserved = 0; reserved < 11; ++reserved) {
    put(out, reserved == 1 ? cry_flags_of(srgb) : 0);
  }
  put(out, kPixelFormatSize);
  put(out, kPixelFlags);
  put(out, 0);  // no FourCC
  put(out, 32);
  put(out, 0x00FF0000);
  put(out, 0x0000FF00);
  put(out, 0x000000FF);
  put(out, 0xFF000000);
  put(out, kCaps);
  put(out, 0);
  put(out, 0);
  put(out, 0);
  put(out, kCryMarker);  // dwReserved2 (byte 124): the flags above are CryEngine's
  Image level = image;
  for (int mip = 0; mip < mips; ++mip) {
    out.insert(out.end(), level.bgra.begin(), level.bgra.end());
    if (mip + 1 < mips) {
      level = half_of(level);
    }
  }
  // The engine's extension after the pixels: no chunks.
  put(out, kExtensionStart);
  put(out, kExtensionEnd);
  put(out, 0);
  return out;
}

}  // namespace kcds::runtime::studio::texture
