#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

// The icon of an item type a resource registers, fitted to the game's own icons.
// Every icon of the game's items is 64 x 64 (Libs/UI/Textures/Icons/Items/*_icon.dds, BC7), and
// the inventory draws a picture at its own size, so a larger one covers the row's name. A .dds
// of 64 x 64 is used as it is, in any format; an uncompressed 32-bit one of another size (up to
// 4096 on a side) is scaled to 64 x 64 (each pixel the average of the source area it covers);
// anything else is refused.
namespace kcd2s::integration {

inline constexpr std::uint32_t kItemIconSize = 64;
inline constexpr std::uint32_t kMaxItemIconSource = 4096;

namespace item_icon_detail {

inline std::uint32_t read32(const std::vector<std::uint8_t>& bytes, std::size_t at) {
  std::uint32_t value = 0;
  std::memcpy(&value, bytes.data() + at, sizeof(value));
  return value;
}

inline void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(std::uint8_t(value >> shift));
  }
}

// One 8-bit channel of a 32-bit pixel: its mask's shift, or -1 for a channel the pixel lacks.
inline int channel_shift(std::uint32_t mask) {
  for (int shift = 0; shift <= 24; shift += 8) {
    if (mask == (0xFFu << shift)) {
      return shift;
    }
  }
  return -1;
}

}  // namespace item_icon_detail

// A .dds as the fitting reads it: its size and, for an uncompressed 32-bit one, where its
// channels sit and where its pixels start.
struct DdsPicture {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  bool rgba32 = false;
  int red = -1, green = -1, blue = -1, alpha = -1;  // shifts in a pixel; alpha -1 for none
  std::size_t pixels = 0;
};

// False for anything that is not a .dds.
inline bool read_dds(const std::vector<std::uint8_t>& bytes, DdsPicture& picture) {
  using item_icon_detail::channel_shift;
  using item_icon_detail::read32;
  constexpr std::size_t kHeader = 4 + 124;
  if (bytes.size() < kHeader || std::memcmp(bytes.data(), "DDS ", 4) != 0 ||
      read32(bytes, 4) != 124) {
    return false;
  }
  DdsPicture read;
  read.height = read32(bytes, 12);
  read.width = read32(bytes, 16);
  const std::uint32_t format_flags = read32(bytes, 80);
  const std::uint32_t four_cc = read32(bytes, 84);
  constexpr std::uint32_t kFourCc = 0x4;
  constexpr std::uint32_t kRgb = 0x40;
  constexpr std::uint32_t kAlphaPixels = 0x1;
  constexpr std::uint32_t kDx10 = 0x30315844;  // "DX10"
  read.pixels = kHeader;
  if ((format_flags & kFourCc) != 0 && four_cc == kDx10) {
    if (bytes.size() < kHeader + 20) {
      return false;
    }
    read.pixels = kHeader + 20;
    const std::uint32_t dxgi = read32(bytes, kHeader);
    if (dxgi == 28 || dxgi == 29) {  // R8G8B8A8
      read.rgba32 = true;
      read.red = 0, read.green = 8, read.blue = 16, read.alpha = 24;
    } else if (dxgi == 87 || dxgi == 91) {  // B8G8R8A8
      read.rgba32 = true;
      read.red = 16, read.green = 8, read.blue = 0, read.alpha = 24;
    } else if (dxgi == 88 || dxgi == 93) {  // B8G8R8X8
      read.rgba32 = true;
      read.red = 16, read.green = 8, read.blue = 0;
    }
  } else if ((format_flags & kRgb) != 0 && read32(bytes, 88) == 32) {
    read.red = channel_shift(read32(bytes, 92));
    read.green = channel_shift(read32(bytes, 96));
    read.blue = channel_shift(read32(bytes, 100));
    read.alpha = (format_flags & kAlphaPixels) != 0 ? channel_shift(read32(bytes, 104)) : -1;
    read.rgba32 = read.red >= 0 && read.green >= 0 && read.blue >= 0;
  }
  if (read.width == 0 || read.height == 0) {
    return false;
  }
  if (read.rgba32 &&
      bytes.size() < read.pixels + std::size_t(read.width) * std::size_t(read.height) * 4) {
    return false;
  }
  picture = read;
  return true;
}

// Whether the client can make an item icon of a .dds (see the top).
inline bool fits_item_icon(const DdsPicture& picture) {
  if (picture.width == kItemIconSize && picture.height == kItemIconSize) {
    return true;
  }
  return picture.rgba32 && picture.width <= kMaxItemIconSource &&
         picture.height <= kMaxItemIconSource;
}

namespace item_icon_detail {

// The header of an uncompressed B8G8R8A8 .dds of `size` x `size` with `mips` levels.
inline void write_header(std::vector<std::uint8_t>& out, std::uint32_t size, std::uint32_t mips) {
  out.insert(out.end(), {'D', 'D', 'S', ' '});
  put32(out, 124);
  // caps, height, width, pitch, pixel format, and the mip count when there are levels
  put32(out, 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | (mips > 1 ? 0x20000u : 0u));
  put32(out, size);
  put32(out, size);
  put32(out, size * 4);
  put32(out, 0);  // depth
  put32(out, mips);
  for (int reserved = 0; reserved < 11; ++reserved) {
    put32(out, 0);
  }
  put32(out, 32);    // pixel format size
  put32(out, 0x41);  // RGB, alpha
  put32(out, 0);
  put32(out, 32);
  put32(out, 0x00FF0000);
  put32(out, 0x0000FF00);
  put32(out, 0x000000FF);
  put32(out, 0xFF000000);
  // a texture; with levels also complex and mipmapped
  put32(out, 0x1000 | (mips > 1 ? 0x400008u : 0u));
  for (int caps = 0; caps < 4; ++caps) {
    put32(out, 0);
  }
}

// The source's pixels scaled to `size` x `size` B8G8R8A8, each the average of the area it covers.
inline std::vector<std::uint8_t> scale_pixels(const std::vector<std::uint8_t>& source,
                                              const DdsPicture& picture, std::uint32_t size);

}  // namespace item_icon_detail

// The .dds an item icon is shown from: the source as it is at 64 x 64 (`size`), else scaled to an
// uncompressed one of that size (B8G8R8A8); empty when it cannot be. A map icon's small copy
// is made the same way at 32.
inline std::vector<std::uint8_t> fit_item_icon(const std::vector<std::uint8_t>& source,
                                               std::uint32_t size = kItemIconSize) {
  DdsPicture picture;
  if (!read_dds(source, picture)) {
    return {};
  }
  if (picture.width == size && picture.height == size) {
    return source;
  }
  if (!picture.rgba32 || picture.width > kMaxItemIconSource ||
      picture.height > kMaxItemIconSource) {
    return {};
  }
  std::vector<std::uint8_t> out;
  out.reserve(128 + size * size * 4);
  item_icon_detail::write_header(out, size, 1);
  const std::vector<std::uint8_t> pixels = item_icon_detail::scale_pixels(source, picture, size);
  out.insert(out.end(), pixels.begin(), pixels.end());
  return out;
}

// A map icon: 64 x 64 B8G8R8A8 with every smaller level down to 1 x 1. The compass
// draws the icon a few pixels wide, and without the smaller levels it came out as a ragged blot
//. Each level halves the one above, its colour weighted by alpha so transparent pixels do
// not darken the edges. A compressed source of 64 x 64 is used as it is (its levels its own); empty
// when the source cannot be used.
inline std::vector<std::uint8_t> map_icon(const std::vector<std::uint8_t>& source) {
  constexpr std::uint32_t kSize = kItemIconSize;
  DdsPicture picture;
  if (!read_dds(source, picture)) {
    return {};
  }
  if (!picture.rgba32) {
    return picture.width == kSize && picture.height == kSize ? source : std::vector<std::uint8_t>{};
  }
  if (picture.width > kMaxItemIconSource || picture.height > kMaxItemIconSource) {
    return {};
  }
  std::uint32_t mips = 1;
  for (std::uint32_t side = kSize; side > 1; side /= 2) {
    ++mips;
  }
  std::vector<std::uint8_t> level = item_icon_detail::scale_pixels(source, picture, kSize);
  std::vector<std::uint8_t> out;
  item_icon_detail::write_header(out, kSize, mips);
  out.insert(out.end(), level.begin(), level.end());
  for (std::uint32_t side = kSize / 2; side >= 1; side /= 2) {
    const std::uint32_t above = side * 2;
    std::vector<std::uint8_t> next(std::size_t(side) * side * 4);
    for (std::uint32_t y = 0; y < side; ++y) {
      for (std::uint32_t x = 0; x < side; ++x) {
        std::uint64_t color[3] = {};
        std::uint64_t alpha = 0;
        for (std::uint32_t dy = 0; dy < 2; ++dy) {
          for (std::uint32_t dx = 0; dx < 2; ++dx) {
            const std::uint8_t* pixel =
                &level[(std::size_t(y * 2 + dy) * above + (x * 2 + dx)) * 4];
            for (int c = 0; c < 3; ++c) {
              color[c] += std::uint64_t(pixel[c]) * pixel[3];
            }
            alpha += pixel[3];
          }
        }
        std::uint8_t* target = &next[(std::size_t(y) * side + x) * 4];
        for (int c = 0; c < 3; ++c) {
          target[c] = alpha == 0 ? 0 : std::uint8_t(color[c] / alpha);
        }
        target[3] = std::uint8_t(alpha / 4);
      }
    }
    out.insert(out.end(), next.begin(), next.end());
    level = std::move(next);
    if (side == 1) {
      break;
    }
  }
  return out;
}

inline std::vector<std::uint8_t> item_icon_detail::scale_pixels(
    const std::vector<std::uint8_t>& source, const DdsPicture& picture, std::uint32_t size) {
  std::vector<std::uint8_t> out;
  out.reserve(std::size_t(size) * size * 4);
  auto channel = [](std::uint32_t pixel, int shift, std::uint32_t none) {
    return shift < 0 ? none : (pixel >> shift) & 0xFFu;
  };
  for (std::uint32_t y = 0; y < size; ++y) {
    const std::uint32_t top = y * picture.height / size;
    std::uint32_t bottom = (y + 1) * picture.height / size;
    bottom = bottom > top ? bottom : top + 1;
    for (std::uint32_t x = 0; x < size; ++x) {
      const std::uint32_t left = x * picture.width / size;
      std::uint32_t right = (x + 1) * picture.width / size;
      right = right > left ? right : left + 1;
      std::uint64_t sum[4] = {};
      std::uint64_t count = 0;
      for (std::uint32_t sy = top; sy < bottom; ++sy) {
        for (std::uint32_t sx = left; sx < right; ++sx) {
          const std::uint32_t pixel =
              read32(source, picture.pixels + (std::size_t(sy) * picture.width + sx) * 4);
          sum[0] += channel(pixel, picture.blue, 0);
          sum[1] += channel(pixel, picture.green, 0);
          sum[2] += channel(pixel, picture.red, 0);
          sum[3] += channel(pixel, picture.alpha, 0xFF);
          count += 1;
        }
      }
      for (const std::uint64_t value : sum) {
        out.push_back(std::uint8_t(value / count));
      }
    }
  }
  return out;
}

// An uncompressed `size` x `size` .dds with nothing in it (every pixel transparent): a map icon's
// small copy, which the map draws beside the icon as the game's own shadow.
inline std::vector<std::uint8_t> transparent_icon(std::uint32_t size) {
  std::vector<std::uint8_t> out;
  out.reserve(128 + size * size * 4);
  item_icon_detail::write_header(out, size, 1);
  out.resize(out.size() + std::size_t(size) * size * 4, 0);
  return out;
}

}  // namespace kcd2s::integration
