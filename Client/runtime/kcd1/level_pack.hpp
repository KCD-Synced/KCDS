// The packs the KCD1 map is read from, built on this PC from the player's own copy of Kingdom
// Come: Deliverance into %LOCALAPPDATA%\KCDS\kcd1 (never shipped, never sent anywhere):
//
//   - kcd1_rataje.pak, the level (level_files.hpp): KCD2's level description and KCD1's compiled
//     world, its entries copied as KCD1 stores them;
//   - materials.pak, the materials/ part of KCD1's GameData pack (the terrain's detail materials
//     and others its objects name), without KCD1's scripts and tables.
//
// A build is kept with the sizes and times of its sources and done again only when one changed.
// KCD1's Objects, Buildings and Textures packs are opened where they are (kcd1_map.cpp).
// Any thread; nothing of the game is called.
#pragma once

#include <filesystem>
#include <optional>

namespace kcds::runtime::kcd1 {

using Logger = void (*)(const char* format, ...);

struct Packs {
  std::filesystem::path level;
  std::filesystem::path materials;
};

// Builds the packs when needed; `kcd1` is that game's folder, `kcd2` this game's. nullopt when a
// source is missing or a pack could not be written.
std::optional<Packs> build_packs(const std::filesystem::path& kcd1,
                                 const std::filesystem::path& kcd2, Logger log);

}  // namespace kcds::runtime::kcd1
