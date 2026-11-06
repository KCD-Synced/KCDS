// The studio's .dds writer (studio/texture_dds.hpp): header, mip levels, box filter.
#include "texture_dds.hpp"

#include <cstring>

#include "kcd2s/testkit.hpp"

namespace {

namespace texture = kcds::runtime::studio::texture;

std::uint32_t word_at(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  std::uint32_t value = 0;
  std::memcpy(&value, &bytes[offset], 4);
  return value;
}

// A 4 x 2 picture: the left half black, the right half white, alpha full.
texture::Image black_and_white() {
  texture::Image image;
  image.width = 4;
  image.height = 2;
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 4; ++x) {
      const std::uint8_t value = x < 2 ? 0 : 255;
      image.bgra.insert(image.bgra.end(), {value, value, value, 255});
    }
  }
  return image;
}

}  // namespace

KCD2S_TEST(texture_dds, every_level_down_to_one_pixel) {
  KCD2S_CHECK_EQ(texture::mip_count(4, 2), 3);
  KCD2S_CHECK_EQ(texture::mip_count(1, 1), 1);
  KCD2S_CHECK_EQ(texture::mip_count(5, 3), 3);
  const texture::Image half = texture::half_of(black_and_white());
  KCD2S_CHECK_EQ(half.width, 2);
  KCD2S_CHECK_EQ(half.height, 1);
  KCD2S_CHECK_EQ(int(half.bgra[0]), 0);
  KCD2S_CHECK_EQ(int(half.bgra[4]), 255);
}

KCD2S_TEST(texture_dds, the_header_and_the_levels_in_order) {
  const auto dds = texture::dds_of(black_and_white());
  // 128 bytes of header, then 4 x 2, 2 x 1 and 1 x 1 pixels, then the empty CExt ... CEnd.
  KCD2S_CHECK_EQ(dds.size(), std::size_t(128 + (8 + 2 + 1) * 4 + 12));
  KCD2S_CHECK_EQ(word_at(dds, 124), 0x46595243u);              // 'CRYF'
  KCD2S_CHECK_EQ(word_at(dds, 36), 0u);                        // no sRGB
  KCD2S_CHECK_EQ(word_at(dds, dds.size() - 12), 0x74784543u);  // 'CExt'
  KCD2S_CHECK_EQ(word_at(dds, dds.size() - 8), 0x646E4543u);   // 'CEnd'
  KCD2S_CHECK_EQ(word_at(texture::dds_of(black_and_white(), true), 36), 0x800u);
  KCD2S_CHECK_EQ(word_at(dds, 0), 0x20534444u);
  KCD2S_CHECK_EQ(word_at(dds, 4), 124u);
  KCD2S_CHECK_EQ(word_at(dds, 12), 2u);
  KCD2S_CHECK_EQ(word_at(dds, 16), 4u);
  KCD2S_CHECK_EQ(word_at(dds, 20), 16u);
  KCD2S_CHECK_EQ(word_at(dds, 28), 3u);
  KCD2S_CHECK_EQ(word_at(dds, 76), 32u);
  KCD2S_CHECK_EQ(word_at(dds, 80), 0x41u);
  KCD2S_CHECK_EQ(word_at(dds, 92), 0x00FF0000u);
  KCD2S_CHECK_EQ(word_at(dds, 104), 0xFF000000u);
  // The last level (before the extension): the mean of black and white.
  KCD2S_CHECK_EQ(int(dds[dds.size() - 16]), 128);
  KCD2S_CHECK(texture::dds_of(texture::Image{}).empty());
}
