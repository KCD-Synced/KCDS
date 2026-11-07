// A model's (.cgf) material and its sub-materials ("slots") read from its material chunk, and the
// game paths the editor may write its own materials and textures at (asset_files.hpp). Header
// only, so the test reads it without the game.
#pragma once

#include <cstdint>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

namespace kcds::runtime::architect::model_slots {

// The material chunk (0x1014): the material's name in 128 bytes, the number of sub-materials, a
// physicalization type each, then their names, each ended by 0.
constexpr std::uint16_t kChunkMaterial = 0x1014;
constexpr std::size_t kMaterialName = 128;
constexpr std::int32_t kMostSlots = 256;

// The model's material and its sub-materials' names, in their ids' order; false when the bytes
// hold no material chunk.
inline bool read(const std::vector<std::uint8_t>& cgf, std::string& material,
                 std::vector<std::string>& slots) {
  const auto u32 = [&cgf](std::size_t at) {
    std::uint32_t value = 0;
    std::memcpy(&value, cgf.data() + at, 4);
    return value;
  };
  if (cgf.size() < 16 || std::memcmp(cgf.data(), "CrCh", 4) != 0) {
    return false;
  }
  const std::uint32_t count = u32(8);
  const std::uint32_t table = u32(12);
  for (std::uint32_t index = 0; index < count; ++index) {
    const std::size_t at = std::size_t(table) + std::size_t(index) * 16;
    if (at + 16 > cgf.size()) {
      return false;
    }
    std::uint16_t kind = 0;
    std::memcpy(&kind, cgf.data() + at, 2);
    const std::uint32_t size = u32(at + 8);
    const std::uint32_t offset = u32(at + 12);
    if (kind != kChunkMaterial || std::size_t(offset) + size > cgf.size() ||
        size < kMaterialName + 4) {
      continue;
    }
    const char* name = reinterpret_cast<const char*>(cgf.data() + offset);
    material.assign(name, strnlen(name, kMaterialName));
    std::int32_t subs = 0;
    std::memcpy(&subs, cgf.data() + offset + kMaterialName, 4);
    if (subs < 0 || subs > kMostSlots) {
      return false;
    }
    std::size_t at_name = std::size_t(offset) + kMaterialName + 4 + std::size_t(subs) * 4;
    const std::size_t end = std::size_t(offset) + size;
    slots.clear();
    for (std::int32_t sub = 0; sub < subs && at_name < end; ++sub) {
      const char* start = reinterpret_cast<const char*>(cgf.data() + at_name);
      const std::size_t length = strnlen(start, end - at_name);
      slots.emplace_back(start, length);
      at_name += length + 1;
    }
    return true;
  }
  return false;
}

// Whether the editor may write a file at this game path: objects/kcds/<folder>/..., lower-case
// letters, digits, '_' and '/', ending .mtl or .dds, no empty folder; models (.cgf) only under
// objects/kcds/opened/ (an opened building's collision and its copy).
inline bool writable(const std::string& path) {
  static const std::regex allowed("^objects/kcds/[a-z0-9_]+/[a-z0-9_/]+\\.(mtl|dds)$");
  static const std::regex model("^objects/kcds/opened/[a-z0-9_]+\\.cgf$");
  return path.size() <= 200 && path.find("//") == std::string::npos &&
         (std::regex_match(path, allowed) || std::regex_match(path, model));
}

}  // namespace kcds::runtime::architect::model_slots
