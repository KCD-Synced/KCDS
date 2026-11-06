#include "cgf_patch.hpp"

#include <algorithm>
#include <cstring>

namespace kcds::runtime::studio::cgf {
namespace {

constexpr std::uint32_t kVersion = 0x746;
constexpr std::uint16_t kMaterialName = 0x1014;
constexpr std::size_t kHeaderSize = 16;
constexpr std::size_t kEntrySize = 16;
constexpr std::size_t kNameSize = 128;

std::uint32_t word_at(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  std::uint32_t value = 0;
  std::memcpy(&value, &bytes[offset], sizeof(value));
  return value;
}

std::uint16_t half_at(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  std::uint16_t value = 0;
  std::memcpy(&value, &bytes[offset], sizeof(value));
  return value;
}

}  // namespace

bool rename_material(std::vector<std::uint8_t>& model, const std::string& material) {
  if (material.empty() || material.size() >= kNameSize || model.size() < kHeaderSize ||
      std::memcmp(model.data(), "CrCh", 4) != 0 || word_at(model, 4) != kVersion) {
    return false;
  }
  const std::uint32_t count = word_at(model, 8);
  const std::uint32_t table = word_at(model, 12);
  if (count > 4096 || std::size_t(table) + std::size_t(count) * kEntrySize > model.size()) {
    return false;
  }
  std::vector<std::size_t> names;
  for (std::uint32_t index = 0; index < count; ++index) {
    const std::size_t entry = table + std::size_t(index) * kEntrySize;
    const std::uint32_t size = word_at(model, entry + 8);
    const std::uint32_t offset = word_at(model, entry + 12);
    if (half_at(model, entry) != kMaterialName) {
      continue;
    }
    if (size < kNameSize || std::size_t(offset) + kNameSize > model.size()) {
      return false;
    }
    names.push_back(offset);
  }
  if (names.empty()) {
    return false;
  }
  for (const std::size_t offset : names) {
    std::fill(model.begin() + std::ptrdiff_t(offset),
              model.begin() + std::ptrdiff_t(offset + kNameSize), std::uint8_t(0));
    std::memcpy(&model[offset], material.data(), material.size());
  }
  return true;
}

}  // namespace kcds::runtime::studio::cgf
