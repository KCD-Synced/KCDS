#pragma once
// The levels a KCDS server can run: the game's own worlds, each with its navigation
// mesh (Data/Levels/<level>/recast.pak) that the players' games use for their people and horses.
// A server started with another name stops at once and says which ones exist, instead of sending
// every player a world their game cannot load. klaster comes with a DLC: a player whose game does
// not have it is told so when joining (Client/runtime/session/server_browser.cpp). A World
// Architect custom map (kcds_map_<name>, Client/runtime/maps/custom_maps.hpp) runs too: every
// player's game needs that map in its KCDS maps folder.
#include <array>
#include <string>

#include "kcd2s/integration/world_config.hpp"

namespace kcd2s::local::config {

inline constexpr std::array<const char*, 3> kLevels = {"trosecko", "kutnohorsko", "klaster"};

inline bool known_level(const std::string& level) {
  for (const char* known : kLevels) {
    if (level == known) {
      return true;
    }
  }
  return kcd2s::integration::custom_map_name(level);
}

// "trosecko, kutnohorsko or klaster", for a message (custom maps are named kcds_map_<name>).
inline std::string level_list() {
  std::string text;
  for (std::size_t i = 0; i < kLevels.size(); ++i) {
    if (i > 0) {
      text += i + 1 == kLevels.size() ? " or " : ", ";
    }
    text += kLevels[i];
  }
  return text;
}

}  // namespace kcd2s::local::config
