#pragma once
// The levels a KCDS server can run: the game's own worlds, each with its navigation
// mesh (Data/Levels/<level>/recast.pak) that the players' games use for their people and horses.
// A server started with another name stops at once and says which ones exist, instead of sending
// every player a world their game cannot load. klaster comes with a DLC: only a player whose
// licence pass vouches for it gets there (host.cjs), and a game without it never loads it
// (Client/runtime/maps/dlc_levels.hpp). A World
// Architect custom map (kcds_map_<name>, Client/runtime/maps/custom_maps.hpp) runs too: every
// player's game needs that map in its KCDS maps folder.
#include <array>
#include <string>
#include <vector>

#include "kcd2s/integration/world_config.hpp"

namespace kcd2s::local::config {

inline constexpr std::array<const char*, 3> kLevels = {"trosecko", "kutnohorsko", "klaster"};

// The Kingdom Come: Deliverance map (Rataje). Never shipped: every player's game loads it from its
// own copy of that game (Client/runtime/kcd1), so a server that has it among its maps admits only
// players whose licence pass vouches they own the game on Steam (KCDS_KCD1_MAP).
inline constexpr const char* kKcd1Level = "kcd1_rataje";

inline bool kcd1_level(const std::string& level) {
  return level == kKcd1Level;
}

// Whether any of a server's maps is the Kingdom Come: Deliverance one.
inline bool any_kcd1_level(const std::vector<std::string>& maps) {
  for (const std::string& map : maps) {
    if (kcd1_level(map)) {
      return true;
    }
  }
  return false;
}

inline bool known_level(const std::string& level) {
  for (const char* known : kLevels) {
    if (level == known) {
      return true;
    }
  }
  return kcd1_level(level) || kcd2s::integration::custom_map_name(level);
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
