// The Asset/Animation Studio's own empty level, kcds_studio, made in memory from the player's own
// game files, as Dev/tools/assets/kcds_studio/make_studio_level.py makes it on disk: klaster's
// level description with every layer, building, vegetation and entity left out, its tables, and
// a flat 128 m floor of the studio's own (heightmap, ground texture, no vis areas, a root-only
// distant detail). The game cannot load a level without a heightmap and vis areas.
//
// studio_scene.cpp hands the result to the game from memory (assets/memory_paks) when no sealed
// copy (%LOCALAPPDATA%\KCDS\studio\kcds_studio.kcdsasset) is there: a player's game never has one.
// Plain code, no game calls.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "../files/zip_read.hpp"

namespace kcds::runtime::studio::level {

// The level's pak (a zip with stored entries) from klaster's level.pak read through `read`
// (`size` bytes); nullopt when a file the level needs is missing or does not read.
std::optional<std::vector<std::uint8_t>> build(const files::ZipReadAt& read, std::uint64_t size);

// The studio's own ground files, for the tests.
std::vector<std::uint8_t> terrain();
std::vector<std::uint8_t> ground_texture();

}  // namespace kcds::runtime::studio::level
