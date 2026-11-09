#include "pe_sections.hpp"

#include <cstring>
#include <vector>

#include "kcd2s/testkit.hpp"

namespace {

using kcds::game_client::ExecutableSections;

// A minimal PE image: the DOS header, the NT signature and file header, no optional header, and
// a section table of .text (code) at 0x1000 and .rdata (data) at 0x3000.
std::vector<std::uint8_t> image_with_sections() {
  std::vector<std::uint8_t> image(0x5000, 0);
  image[0] = 'M';
  image[1] = 'Z';
  const std::uint32_t nt = 0x80;
  std::memcpy(&image[0x3C], &nt, 4);
  std::memcpy(&image[nt], "PE\0\0", 4);
  const std::uint16_t sections = 2;
  const std::uint16_t optional = 0;
  std::memcpy(&image[nt + 6], &sections, 2);
  std::memcpy(&image[nt + 20], &optional, 2);
  const auto section = [&](std::size_t index, const char* name, std::uint32_t address,
                           std::uint32_t size, std::uint32_t characteristics) {
    const std::size_t entry = nt + 24 + index * 40;
    std::memcpy(&image[entry], name, std::strlen(name));
    std::memcpy(&image[entry + 8], &size, 4);
    std::memcpy(&image[entry + 12], &address, 4);
    std::memcpy(&image[entry + 36], &characteristics, 4);
  };
  section(0, ".text", 0x1000, 0x1800, 0x60000020);
  section(1, ".rdata", 0x3000, 0x1000, 0x40000040);
  return image;
}

}  // namespace

KCD2S_TEST(pe_sections, only_executable_sections_hold_functions) {
  const auto image = image_with_sections();
  const auto code = ExecutableSections::of(image.data(), image.size());
  KCD2S_CHECK_EQ(code.count(), std::size_t(1));
  KCD2S_CHECK(code.contains(0x1000));
  KCD2S_CHECK(code.contains(0x27FF));
  KCD2S_CHECK(!code.contains(0x2800));
  KCD2S_CHECK(!code.contains(0x3000));
  KCD2S_CHECK(!code.contains(0x10));
}

KCD2S_TEST(pe_sections, an_image_without_headers_has_no_code) {
  std::vector<std::uint8_t> image(0x100, 0);
  KCD2S_CHECK_EQ(ExecutableSections::of(image.data(), image.size()).count(), std::size_t(0));
  KCD2S_CHECK_EQ(ExecutableSections::of(nullptr, 0).count(), std::size_t(0));
  auto broken = image_with_sections();
  broken[0x80] = 'X';
  KCD2S_CHECK_EQ(ExecutableSections::of(broken.data(), broken.size()).count(), std::size_t(0));
}
