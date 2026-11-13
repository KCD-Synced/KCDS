// The packs the KCD1 map is read from, built on this PC from the player's own copy of Kingdom
// Come: Deliverance into %LOCALAPPDATA%\KCDS\kcd1 (never shipped, never sent anywhere):
//
//   - kcd1_rataje.pak, the level (level_files.hpp): KCD2's level description and KCD1's compiled
//     world, its entries copied as KCD1 stores them;
//   - assets.pak, the materials/ part of KCD1's GameData pack (the terrain's detail materials
//     and others its objects name), without KCD1's scripts and tables, and its world's textures
//     from its Textures packs, without the engine's own folders (level_files.hpp texture_file).
//
// A build is kept with the sizes and times of its sources and done again only when one changed.
// KCD1's Objects and Buildings packs are opened where they are (kcd1_map.cpp).
// Any thread; nothing of the game is called.
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace kcds::runtime::kcd1 {

using Logger = void (*)(const char* format, ...);

struct Packs {
  std::filesystem::path level;
  std::filesystem::path assets;
  // The states KCD1's layers start in (level_files.hpp layer_starts), a text file.
  std::filesystem::path layer_starts;
};

// Builds the packs when needed; `kcd1` is that game's folder, `kcd2` this game's. nullopt when a
// source is missing or a pack could not be written.
std::optional<Packs> build_packs(const std::filesystem::path& kcd1,
                                 const std::filesystem::path& kcd2, Logger log);

// A file of a pack on disk as text (`name` in any case, either slash); empty when it is not there.
std::string pack_text(const std::filesystem::path& pack, const std::string& name);

}  // namespace kcds::runtime::kcd1
