#include <cstdint>
#include <cstring>
#include <vector>

#include "../dds_picture.hpp"
#include "../png_file.hpp"
#include "kcd2s/testkit.hpp"

using namespace kcds::runtime::web::icons;

namespace {

// Writes a block's bits from the lowest bit of its first byte up, as BC7 reads them.
struct BlockWriter {
  std::uint8_t block[16] = {};
  int position = 0;
  void put(std::uint32_t value, int count) {
    for (int bit = 0; bit < count; ++bit) {
      if ((value >> bit) & 1) {
        block[(position + bit) >> 3] |= std::uint8_t(1 << ((position + bit) & 7));
      }
    }
    position += count;
  }
};

// A mode 6 block: one subset, 7-bit RGBA endpoints with a p-bit each, 4-bit indices. Every pixel
// takes `index` except the last, which takes 15 (the second endpoint).
std::vector<std::uint8_t> mode6_block(const std::uint32_t first[4], const std::uint32_t second[4],
                                      std::uint32_t index) {
  BlockWriter w;
  w.put(1 << 6, 7);
  for (int channel = 0; channel < 4; ++channel) {
    w.put(first[channel], 7);
    w.put(second[channel], 7);
  }
  w.put(1, 1);  // first endpoint's p-bit
  w.put(1, 1);  // second endpoint's p-bit
  w.put(index & 7, 3);
  for (int pixel = 1; pixel < 15; ++pixel) {
    w.put(index, 4);
  }
  w.put(15, 4);
  return std::vector<std::uint8_t>(w.block, w.block + 16);
}

std::vector<std::uint8_t> dds_header(std::uint32_t width, std::uint32_t height, const char* code,
                                     std::uint32_t dxgi = 0) {
  std::vector<std::uint8_t> file(128, 0);
  std::memcpy(file.data(), "DDS ", 4);
  auto put = [&](std::size_t at, std::uint32_t value) { std::memcpy(file.data() + at, &value, 4); };
  put(4, 124);
  put(12, height);
  put(16, width);
  put(76, 32);
  put(80, 0x4);  // four-character code
  std::memcpy(file.data() + 84, code, 4);
  if (dxgi) {
    std::vector<std::uint8_t> dx10(20, 0);
    std::memcpy(dx10.data(), &dxgi, 4);
    dx10[4] = 3;  // a 2D texture
    file.insert(file.end(), dx10.begin(), dx10.end());
  }
  return file;
}

std::uint32_t big32(const std::vector<std::uint8_t>& bytes, std::size_t at) {
  return (std::uint32_t(bytes[at]) << 24) | (std::uint32_t(bytes[at + 1]) << 16) |
         (std::uint32_t(bytes[at + 2]) << 8) | bytes[at + 3];
}

}  // namespace

KCD2S_TEST(web_icons, bc7_mode6_takes_both_endpoints_and_their_p_bits) {
  const std::uint32_t red[4] = {127, 0, 0, 127};
  const std::uint32_t blue[4] = {0, 0, 127, 0};
  const auto block = mode6_block(red, blue, 0);
  std::uint8_t pixels[64];
  decode_bc7_block(block.data(), pixels);
  // (127 << 1) | 1 = 255 and (0 << 1) | 1 = 1: the p-bit is the lowest bit of each component.
  KCD2S_CHECK_EQ(int(pixels[0]), 255);
  KCD2S_CHECK_EQ(int(pixels[1]), 1);
  KCD2S_CHECK_EQ(int(pixels[2]), 1);
  KCD2S_CHECK_EQ(int(pixels[3]), 255);
  KCD2S_CHECK_EQ(int(pixels[60]), 1);
  KCD2S_CHECK_EQ(int(pixels[62]), 255);
  KCD2S_CHECK_EQ(int(pixels[63]), 1);
}

KCD2S_TEST(web_icons, bc7_index_weights_mix_the_endpoints) {
  const std::uint32_t black[4] = {0, 0, 0, 127};
  const std::uint32_t white[4] = {127, 127, 127, 127};
  // Index 8 of 4-bit weights is 34 of 64: (30 * 1 + 34 * 255 + 32) >> 6 = 136.
  const auto block = mode6_block(black, white, 8);
  std::uint8_t pixels[64];
  decode_bc7_block(block.data(), pixels);
  KCD2S_CHECK_EQ(int(pixels[4]), 136);
  KCD2S_CHECK_EQ(int(pixels[7]), 255);
}

KCD2S_TEST(web_icons, a_reserved_bc7_block_is_transparent_black) {
  const std::uint8_t block[16] = {};
  std::uint8_t pixels[64];
  std::memset(pixels, 0x55, sizeof(pixels));
  decode_bc7_block(block, pixels);
  bool all_zero = true;
  for (const std::uint8_t value : pixels) {
    all_zero = all_zero && value == 0;
  }
  KCD2S_CHECK(all_zero);
}

KCD2S_TEST(web_icons, a_dx10_bc7_file_decodes_and_a_short_one_is_refused) {
  const std::uint32_t green[4] = {0, 127, 0, 127};
  const auto block = mode6_block(green, green, 0);
  auto file = dds_header(4, 4, "DX10", 98);
  file.insert(file.end(), block.begin(), block.end());
  Picture picture;
  KCD2S_CHECK(decode_dds(file, picture));
  KCD2S_CHECK_EQ(picture.width, 4u);
  KCD2S_CHECK_EQ(picture.rgba.size(), std::size_t(64));
  KCD2S_CHECK_EQ(int(picture.rgba[1]), 255);
  file.pop_back();
  KCD2S_CHECK(!decode_dds(file, picture));
  // A picture whose size is not a multiple of 4 keeps only its own pixels.
  auto small = dds_header(2, 3, "DX10", 98);
  small.insert(small.end(), block.begin(), block.end());
  KCD2S_CHECK(decode_dds(small, picture));
  KCD2S_CHECK_EQ(picture.rgba.size(), std::size_t(2 * 3 * 4));
}

KCD2S_TEST(web_icons, dxt1_and_dxt5_blocks_decode) {
  // DXT1: white and black endpoints, the first row white, the rest index 3 (one third white).
  auto dxt1 = dds_header(4, 4, "DXT1");
  const std::uint8_t colour[8] = {0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF};
  dxt1.insert(dxt1.end(), colour, colour + 8);
  Picture picture;
  KCD2S_CHECK(decode_dds(dxt1, picture));
  KCD2S_CHECK_EQ(int(picture.rgba[0]), 255);
  KCD2S_CHECK_EQ(int(picture.rgba[16]), 85);
  KCD2S_CHECK_EQ(int(picture.rgba[19]), 255);
  // DXT5: alpha endpoints 200 and 100, every alpha index 1 (the second endpoint).
  auto dxt5 = dds_header(4, 4, "DXT5");
  const std::uint8_t alpha[8] = {200, 100, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24};
  dxt5.insert(dxt5.end(), alpha, alpha + 8);
  dxt5.insert(dxt5.end(), colour, colour + 8);
  KCD2S_CHECK(decode_dds(dxt5, picture));
  KCD2S_CHECK_EQ(int(picture.rgba[3]), 100);
  KCD2S_CHECK_EQ(int(picture.rgba[63]), 100);
}

KCD2S_TEST(web_icons, a_bgra_file_is_turned_to_rgba) {
  std::vector<std::uint8_t> file(128, 0);
  std::memcpy(file.data(), "DDS ", 4);
  const std::uint32_t fields[][2] = {{12, 1}, {16, 1}, {80, 0x41}, {88, 32}, {92, 0x00FF0000}};
  for (const auto& field : fields) {
    std::memcpy(file.data() + field[0], &field[1], 4);
  }
  const std::uint8_t bgra[4] = {10, 20, 30, 40};
  file.insert(file.end(), bgra, bgra + 4);
  Picture picture;
  KCD2S_CHECK(decode_dds(file, picture));
  KCD2S_CHECK_EQ(int(picture.rgba[0]), 30);
  KCD2S_CHECK_EQ(int(picture.rgba[2]), 10);
  KCD2S_CHECK_EQ(int(picture.rgba[3]), 40);
  KCD2S_CHECK(!decode_dds(std::vector<std::uint8_t>(64, 0), picture));
}

KCD2S_TEST(web_icons, png_file_has_its_chunks_and_the_pixels_stored) {
  Picture picture;
  picture.width = 2;
  picture.height = 1;
  picture.rgba = {1, 2, 3, 4, 5, 6, 7, 8};
  const auto png = png_file(picture);
  const std::uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  KCD2S_CHECK(png.size() > 8 && std::memcmp(png.data(), signature, 8) == 0);
  KCD2S_CHECK_EQ(big32(png, 8), 13u);  // IHDR length
  KCD2S_CHECK(std::memcmp(png.data() + 12, "IHDR", 4) == 0);
  KCD2S_CHECK_EQ(big32(png, 16), 2u);
  KCD2S_CHECK_EQ(big32(png, 20), 1u);
  // IEND and its fixed CRC close the file.
  KCD2S_CHECK(std::memcmp(png.data() + png.size() - 8, "IEND", 4) == 0);
  KCD2S_CHECK_EQ(big32(png, png.size() - 4), 0xAE426082u);
  // The row (filter byte, then the pixels) is in the stored deflate block.
  const std::uint8_t row[9] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
  bool found = false;
  for (std::size_t at = 0; at + 9 <= png.size(); ++at) {
    found = found || std::memcmp(png.data() + at, row, 9) == 0;
  }
  KCD2S_CHECK(found);
  picture.rgba.pop_back();
  KCD2S_CHECK(png_file(picture).empty());
}
