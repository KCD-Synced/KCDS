// The executable sections of a loaded PE image (WHGame.dll), read from its own headers: a
// signature's match is a function only when it lands in one of them. A game patch can make an old
// pattern match bytes of data or of another section; the landing check refuses that instead of
// hooking it. Portable (reads bytes only), for the unit tests.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace kcds::game_client {

class ExecutableSections {
 public:
  // Reads the section table of the image loaded at `image` (`size` bytes). Without readable
  // headers it holds no section, and contains() is false everywhere.
  static ExecutableSections of(const std::uint8_t* image, std::size_t size) {
    ExecutableSections sections;
    const auto u16 = [&](std::size_t at) -> std::uint32_t {
      std::uint16_t value = 0;
      std::memcpy(&value, image + at, sizeof(value));
      return value;
    };
    const auto u32 = [&](std::size_t at) -> std::uint32_t {
      std::uint32_t value = 0;
      std::memcpy(&value, image + at, sizeof(value));
      return value;
    };
    if (!image || size < 0x40 || image[0] != 'M' || image[1] != 'Z') {
      return sections;
    }
    const std::size_t nt = u32(0x3C);
    if (nt + 24 > size || std::memcmp(image + nt, "PE\0\0", 4) != 0) {
      return sections;
    }
    const std::size_t count = u16(nt + 6);
    const std::size_t optional = u16(nt + 20);
    const std::size_t table = nt + 24 + optional;
    for (std::size_t i = 0; i < count; ++i) {
      const std::size_t entry = table + i * kSectionHeaderBytes;
      if (entry + kSectionHeaderBytes > size) {
        break;
      }
      const std::uint32_t virtual_size = u32(entry + 8);
      const std::uint32_t address = u32(entry + 12);
      const std::uint32_t characteristics = u32(entry + 36);
      if ((characteristics & kExecutable) == 0 || address >= size) {
        continue;
      }
      const std::size_t end = std::size_t(address) + virtual_size;
      sections.ranges_.emplace_back(address, end < size ? end : size);
    }
    return sections;
  }

  // Whether the image offset lies in an executable section.
  bool contains(std::size_t offset) const {
    for (const auto& [begin, end] : ranges_) {
      if (offset >= begin && offset < end) {
        return true;
      }
    }
    return false;
  }

  std::size_t count() const {
    return ranges_.size();
  }

 private:
  static constexpr std::size_t kSectionHeaderBytes = 40;
  static constexpr std::uint32_t kExecutable = 0x20000000;  // IMAGE_SCN_MEM_EXECUTE
  std::vector<std::pair<std::size_t, std::size_t>> ranges_;
};

}  // namespace kcds::game_client
