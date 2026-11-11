#include "dds_picture.hpp"

#include <algorithm>
#include <cstring>

namespace kcds::runtime::web::icons {
namespace {

constexpr std::size_t kHeaderBytes = 128;  // "DDS " and the 124-byte header
constexpr std::size_t kDx10Bytes = 20;

// DXGI formats the decoder reads.
constexpr std::uint32_t kDxgiRgba8 = 28;
constexpr std::uint32_t kDxgiRgba8Srgb = 29;
constexpr std::uint32_t kDxgiBc1 = 71;
constexpr std::uint32_t kDxgiBc1Srgb = 72;
constexpr std::uint32_t kDxgiBc3 = 77;
constexpr std::uint32_t kDxgiBc3Srgb = 78;
constexpr std::uint32_t kDxgiBgra8 = 87;
constexpr std::uint32_t kDxgiBgra8Srgb = 91;
constexpr std::uint32_t kDxgiBc7 = 98;
constexpr std::uint32_t kDxgiBc7Srgb = 99;

enum class Format { Unknown, Bc1, Bc3, Bc7, Rgba8, Bgra8 };

std::uint32_t read32(const std::uint8_t* at) {
  std::uint32_t value = 0;
  std::memcpy(&value, at, 4);
  return value;
}

std::uint32_t fourcc(const char* text) {
  return std::uint32_t(std::uint8_t(text[0])) | (std::uint32_t(std::uint8_t(text[1])) << 8) |
         (std::uint32_t(std::uint8_t(text[2])) << 16) |
         (std::uint32_t(std::uint8_t(text[3])) << 24);
}

Format dxgi_format(std::uint32_t dxgi) {
  switch (dxgi) {
    case kDxgiBc1:
    case kDxgiBc1Srgb:
      return Format::Bc1;
    case kDxgiBc3:
    case kDxgiBc3Srgb:
      return Format::Bc3;
    case kDxgiBc7:
    case kDxgiBc7Srgb:
      return Format::Bc7;
    case kDxgiRgba8:
    case kDxgiRgba8Srgb:
      return Format::Rgba8;
    case kDxgiBgra8:
    case kDxgiBgra8Srgb:
      return Format::Bgra8;
    default:
      return Format::Unknown;
  }
}

// ---- BC1 / BC3 ----------------------------------------------------------------------------------

void rgb565(std::uint16_t color, std::uint8_t* out) {
  const std::uint32_t red = (color >> 11) & 31;
  const std::uint32_t green = (color >> 5) & 63;
  const std::uint32_t blue = color & 31;
  out[0] = std::uint8_t((red << 3) | (red >> 2));
  out[1] = std::uint8_t((green << 2) | (green >> 4));
  out[2] = std::uint8_t((blue << 3) | (blue >> 2));
  out[3] = 255;
}

// A BC1 colour block. opaque_only: the block of a BC3 texture, always four colours.
void decode_bc1_block(const std::uint8_t* block, std::uint8_t* pixels, bool opaque_only) {
  std::uint16_t first = 0;
  std::uint16_t second = 0;
  std::memcpy(&first, block, 2);
  std::memcpy(&second, block + 2, 2);
  std::uint8_t palette[4][4];
  rgb565(first, palette[0]);
  rgb565(second, palette[1]);
  const bool four_colours = opaque_only || first > second;
  for (int channel = 0; channel < 3; ++channel) {
    const int a = palette[0][channel];
    const int b = palette[1][channel];
    if (four_colours) {
      palette[2][channel] = std::uint8_t((2 * a + b + 1) / 3);
      palette[3][channel] = std::uint8_t((a + 2 * b + 1) / 3);
    } else {
      palette[2][channel] = std::uint8_t((a + b + 1) / 2);
      palette[3][channel] = 0;
    }
  }
  palette[2][3] = 255;
  palette[3][3] = four_colours ? 255 : 0;
  const std::uint32_t indices = read32(block + 4);
  for (int pixel = 0; pixel < 16; ++pixel) {
    const std::uint32_t index = (indices >> (2 * pixel)) & 3;
    std::memcpy(pixels + pixel * 4, palette[index], 4);
  }
}

// A BC3 block: the alpha block, then a four-colour BC1 block.
void decode_bc3_block(const std::uint8_t* block, std::uint8_t* pixels) {
  decode_bc1_block(block + 8, pixels, true);
  const int first = block[0];
  const int second = block[1];
  std::uint8_t alphas[8];
  alphas[0] = std::uint8_t(first);
  alphas[1] = std::uint8_t(second);
  if (first > second) {
    for (int step = 1; step <= 6; ++step) {
      alphas[1 + step] = std::uint8_t(((7 - step) * first + step * second + 3) / 7);
    }
  } else {
    for (int step = 1; step <= 4; ++step) {
      alphas[1 + step] = std::uint8_t(((5 - step) * first + step * second + 2) / 5);
    }
    alphas[6] = 0;
    alphas[7] = 255;
  }
  std::uint64_t indices = 0;
  for (int byte = 0; byte < 6; ++byte) {
    indices |= std::uint64_t(block[2 + byte]) << (8 * byte);
  }
  for (int pixel = 0; pixel < 16; ++pixel) {
    pixels[pixel * 4 + 3] = alphas[(indices >> (3 * pixel)) & 7];
  }
}

// ---- BC7
// -----------------------------------------------------------------------------------------

struct Bc7Mode {
  int subsets;
  int partition_bits;
  int rotation_bits;
  int selection_bits;
  int colour_bits;
  int alpha_bits;
  int endpoint_pbits;  // one p-bit per endpoint
  int shared_pbits;    // one p-bit per subset
  int index_bits;
  int second_index_bits;
};

constexpr Bc7Mode kBc7Modes[8] = {
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
};

// Two-subset partitions: bit i set puts pixel i in the second subset.
constexpr std::uint16_t kPartitions2[64] = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80,
    0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000, 0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310,
    0x3100, 0x8CCE, 0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C, 0xAAAA,
    0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC,
    0x6996, 0xC33C, 0x9966, 0x0660, 0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6,
    0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22,
};

// Three-subset partitions: each pixel's subset, two bits per pixel (pixel 0 lowest).
constexpr std::uint32_t kPartitions3[64] = {
    0xAA685050, 0x6A5A5040, 0x5A5A4200, 0x5450A0A8, 0xA5A50000, 0xA0A05050, 0x5555A0A0, 0x5A5A5050,
    0xAA550000, 0xAA555500, 0xAAAA5500, 0x90909090, 0x94949494, 0xA4A4A4A4, 0xA9A59450, 0x2A0A4250,
    0xA5945040, 0x0A425054, 0xA5A5A500, 0x55A0A0A0, 0xA8A85454, 0x6A6A4040, 0xA4A45000, 0x1A1A0500,
    0x0050A4A4, 0xAAA59090, 0x14696914, 0x69691400, 0xA08585A0, 0xAA821414, 0x50A4A450, 0x6A5A0200,
    0xA9A58000, 0x5090A0A8, 0xA8A09050, 0x24242424, 0x00AA5500, 0x24924924, 0x24499224, 0x50A50A50,
    0x500AA550, 0xAAAA4444, 0x66660000, 0xA5A0A5A0, 0x50A050A0, 0x69286928, 0x44AAAA44, 0x66666600,
    0xAA444444, 0x54A854A8, 0x95809580, 0x96969600, 0xA85454A8, 0x80959580, 0xAA141414, 0x96960000,
    0xAAAA1414, 0xA05050A0, 0xA0A5A5A0, 0x96000000, 0x40804080, 0xA9A8A9A8, 0xAAAAAA44, 0x2A4A5254,
};

// The anchor pixel (its index has one bit less) of a partition's second subset, and of the
// second and third subsets of a three-subset partition. The first subset's is pixel 0.
constexpr std::uint8_t kAnchor2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8, 2,  2, 8,
    8,  15, 2,  8,  2,  2,  8,  8,  2,  2,  15, 15, 6,  8,  2,  8,  15, 15, 2, 8,  2, 2,
    2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2, 15,
};
constexpr std::uint8_t kAnchor3Second[64] = {
    3,  3,  15, 15, 8, 3,  15, 15, 8,  8,  6, 6,  6, 5,  3, 3,  3,  3,  8,  15, 3,  3,
    6,  10, 5,  8,  8, 6,  8,  5,  15, 15, 8, 15, 3, 5,  6, 10, 8,  15, 15, 3,  15, 5,
    15, 15, 15, 15, 3, 15, 5,  5,  5,  8,  5, 10, 5, 10, 8, 13, 15, 12, 3,  3,
};
constexpr std::uint8_t kAnchor3Third[64] = {
    15, 8, 8, 3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,  15, 8,  15, 3,  15, 8,
    15, 8, 3, 15, 6,  10, 15, 15, 10, 8,  15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15,
    3,  6, 6, 8,  15, 3,  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8,
};

constexpr std::uint8_t kWeights2[4] = {0, 21, 43, 64};
constexpr std::uint8_t kWeights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr std::uint8_t kWeights4[16] = {0,  4,  9,  13, 17, 21, 26, 30,
                                        34, 38, 43, 47, 51, 55, 60, 64};

const std::uint8_t* weights_of(int bits) {
  if (bits == 2) {
    return kWeights2;
  }
  if (bits == 3) {
    return kWeights3;
  }
  return kWeights4;
}

// The block's bits from the lowest bit of its first byte up.
class BitStream {
 public:
  explicit BitStream(const std::uint8_t* block) : block_(block) {
  }
  std::uint32_t read(int count) {
    std::uint32_t value = 0;
    for (int bit = 0; bit < count; ++bit) {
      const int at = position_ + bit;
      const std::uint32_t set = (block_[at >> 3] >> (at & 7)) & 1;
      value |= set << bit;
    }
    position_ += count;
    return value;
  }

 private:
  const std::uint8_t* block_;
  int position_ = 0;
};

int subset_of(const Bc7Mode& mode, std::uint32_t partition, int pixel) {
  if (mode.subsets == 2) {
    return (kPartitions2[partition] >> pixel) & 1;
  }
  if (mode.subsets == 3) {
    return int((kPartitions3[partition] >> (2 * pixel)) & 3);
  }
  return 0;
}

bool is_anchor(const Bc7Mode& mode, std::uint32_t partition, int pixel) {
  if (pixel == 0) {
    return true;
  }
  if (mode.subsets == 2) {
    return pixel == kAnchor2[partition];
  }
  if (mode.subsets == 3) {
    return pixel == kAnchor3Second[partition] || pixel == kAnchor3Third[partition];
  }
  return false;
}

// An endpoint component of `bits` bits widened to 8 by repeating its top bits.
std::uint8_t widen(std::uint32_t value, int bits) {
  value <<= 8 - bits;
  return std::uint8_t(value | (value >> bits));
}

std::uint8_t mix(std::uint8_t first, std::uint8_t second, std::uint8_t weight) {
  return std::uint8_t(((64 - weight) * first + weight * second + 32) >> 6);
}

}  // namespace

void decode_bc7_block(const std::uint8_t* block, std::uint8_t* pixels) {
  int mode_number = 0;
  while (mode_number < 8 && ((block[0] >> mode_number) & 1) == 0) {
    ++mode_number;
  }
  if (mode_number == 8) {
    // A reserved mode: the block is transparent black.
    std::memset(pixels, 0, 64);
    return;
  }
  const Bc7Mode& mode = kBc7Modes[mode_number];
  BitStream bits(block);
  bits.read(mode_number + 1);
  const std::uint32_t partition = bits.read(mode.partition_bits);
  const std::uint32_t rotation = bits.read(mode.rotation_bits);
  const std::uint32_t selection = bits.read(mode.selection_bits);

  // Endpoints: [subset * 2 + end][channel], red of every endpoint first, then green, blue, alpha.
  std::uint32_t ends[6][4] = {};
  const int endpoints = mode.subsets * 2;
  for (int channel = 0; channel < 3; ++channel) {
    for (int end = 0; end < endpoints; ++end) {
      ends[end][channel] = bits.read(mode.colour_bits);
    }
  }
  for (int end = 0; end < endpoints; ++end) {
    ends[end][3] = mode.alpha_bits ? bits.read(mode.alpha_bits) : 255;
  }
  int colour_bits = mode.colour_bits;
  int alpha_bits = mode.alpha_bits;
  if (mode.endpoint_pbits || mode.shared_pbits) {
    std::uint32_t pbits[6] = {};
    if (mode.endpoint_pbits) {
      for (int end = 0; end < endpoints; ++end) {
        pbits[end] = bits.read(1);
      }
    } else {
      for (int subset = 0; subset < mode.subsets; ++subset) {
        const std::uint32_t shared = bits.read(1);
        pbits[subset * 2] = shared;
        pbits[subset * 2 + 1] = shared;
      }
    }
    for (int end = 0; end < endpoints; ++end) {
      for (int channel = 0; channel < 3; ++channel) {
        ends[end][channel] = (ends[end][channel] << 1) | pbits[end];
      }
      if (mode.alpha_bits) {
        ends[end][3] = (ends[end][3] << 1) | pbits[end];
      }
    }
    colour_bits += 1;
    if (alpha_bits) {
      alpha_bits += 1;
    }
  }
  std::uint8_t widened[6][4];
  for (int end = 0; end < endpoints; ++end) {
    for (int channel = 0; channel < 3; ++channel) {
      widened[end][channel] = widen(ends[end][channel], colour_bits);
    }
    widened[end][3] = alpha_bits ? widen(ends[end][3], alpha_bits) : 255;
  }

  // Indices: the primary set, then (modes 4 and 5) the secondary set; an anchor pixel's index
  // is one bit shorter (its top bit is zero).
  std::uint32_t primary[16];
  std::uint32_t secondary[16] = {};
  for (int pixel = 0; pixel < 16; ++pixel) {
    const int count = is_anchor(mode, partition, pixel) ? mode.index_bits - 1 : mode.index_bits;
    primary[pixel] = bits.read(count);
  }
  if (mode.second_index_bits) {
    for (int pixel = 0; pixel < 16; ++pixel) {
      const int count = pixel == 0 ? mode.second_index_bits - 1 : mode.second_index_bits;
      secondary[pixel] = bits.read(count);
    }
  }

  for (int pixel = 0; pixel < 16; ++pixel) {
    const int subset = subset_of(mode, partition, pixel);
    const std::uint8_t* first = widened[subset * 2];
    const std::uint8_t* second = widened[subset * 2 + 1];
    std::uint8_t colour_weight = 0;
    std::uint8_t alpha_weight = 0;
    if (!mode.second_index_bits) {
      colour_weight = weights_of(mode.index_bits)[primary[pixel]];
      alpha_weight = colour_weight;
    } else if (selection == 0) {
      colour_weight = weights_of(mode.index_bits)[primary[pixel]];
      alpha_weight = weights_of(mode.second_index_bits)[secondary[pixel]];
    } else {
      colour_weight = weights_of(mode.second_index_bits)[secondary[pixel]];
      alpha_weight = weights_of(mode.index_bits)[primary[pixel]];
    }
    std::uint8_t* out = pixels + pixel * 4;
    for (int channel = 0; channel < 3; ++channel) {
      out[channel] = mix(first[channel], second[channel], colour_weight);
    }
    out[3] = mix(first[3], second[3], alpha_weight);
    // Modes 4 and 5 may store one colour channel in the alpha's place.
    if (rotation != 0) {
      std::swap(out[3], out[rotation - 1]);
    }
  }
}

bool decode_dds(const std::vector<std::uint8_t>& file, Picture& out) {
  if (file.size() < kHeaderBytes || std::memcmp(file.data(), "DDS ", 4) != 0) {
    return false;
  }
  const std::uint32_t height = read32(file.data() + 12);
  const std::uint32_t width = read32(file.data() + 16);
  if (width == 0 || height == 0 || width > kMaxPictureSide || height > kMaxPictureSide) {
    return false;
  }
  const std::uint32_t pixel_flags = read32(file.data() + 80);
  const std::uint32_t code = read32(file.data() + 84);
  std::size_t data_at = kHeaderBytes;
  Format format = Format::Unknown;
  constexpr std::uint32_t kFourCcFlag = 0x4;
  constexpr std::uint32_t kRgbFlag = 0x40;
  if ((pixel_flags & kFourCcFlag) && code == fourcc("DX10")) {
    if (file.size() < kHeaderBytes + kDx10Bytes) {
      return false;
    }
    format = dxgi_format(read32(file.data() + kHeaderBytes));
    data_at += kDx10Bytes;
  } else if (pixel_flags & kFourCcFlag) {
    if (code == fourcc("DXT1")) {
      format = Format::Bc1;
    } else if (code == fourcc("DXT5")) {
      format = Format::Bc3;
    }
  } else if ((pixel_flags & kRgbFlag) && read32(file.data() + 88) == 32) {
    const std::uint32_t red_mask = read32(file.data() + 92);
    if (red_mask == 0x000000FF) {
      format = Format::Rgba8;
    } else if (red_mask == 0x00FF0000) {
      format = Format::Bgra8;
    }
  }
  if (format == Format::Unknown) {
    return false;
  }

  out.width = width;
  out.height = height;
  out.rgba.assign(std::size_t(width) * height * 4, 0);
  const std::uint8_t* data = file.data() + data_at;
  const std::size_t available = file.size() - data_at;

  if (format == Format::Rgba8 || format == Format::Bgra8) {
    const std::size_t needed = std::size_t(width) * height * 4;
    if (available < needed) {
      return false;
    }
    std::memcpy(out.rgba.data(), data, needed);
    if (format == Format::Bgra8) {
      for (std::size_t at = 0; at < needed; at += 4) {
        std::swap(out.rgba[at], out.rgba[at + 2]);
      }
    }
    return true;
  }

  const std::size_t block_bytes = format == Format::Bc1 ? 8 : 16;
  const std::uint32_t blocks_wide = (width + 3) / 4;
  const std::uint32_t blocks_high = (height + 3) / 4;
  if (available < std::size_t(blocks_wide) * blocks_high * block_bytes) {
    return false;
  }
  std::uint8_t pixels[64];
  for (std::uint32_t block_y = 0; block_y < blocks_high; ++block_y) {
    for (std::uint32_t block_x = 0; block_x < blocks_wide; ++block_x) {
      const std::uint8_t* block =
          data + (std::size_t(block_y) * blocks_wide + block_x) * block_bytes;
      if (format == Format::Bc1) {
        decode_bc1_block(block, pixels, false);
      } else if (format == Format::Bc3) {
        decode_bc3_block(block, pixels);
      } else {
        decode_bc7_block(block, pixels);
      }
      // Copy the block's pixels that lie inside the picture.
      for (std::uint32_t row = 0; row < 4; ++row) {
        const std::uint32_t y = block_y * 4 + row;
        if (y >= height) {
          break;
        }
        for (std::uint32_t column = 0; column < 4; ++column) {
          const std::uint32_t x = block_x * 4 + column;
          if (x >= width) {
            break;
          }
          std::memcpy(out.rgba.data() + (std::size_t(y) * width + x) * 4,
                      pixels + (row * 4 + column) * 4, 4);
        }
      }
    }
  }
  return true;
}

}  // namespace kcds::runtime::web::icons
