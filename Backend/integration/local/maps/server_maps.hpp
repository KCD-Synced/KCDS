#pragma once
// The maps one server runs at once. A server runs its level (server.json `level`), or several of
// the game's maps (server.json `maps`, "trosecko,kutnohorsko"): every map is a world of its own in
// the same process, each player is on one of them, and a player who travels gets the other map's
// world config, loads that level and is placed there. The first map is where players start.
// Header only, so the tests read it without a socket.
#include <cstdint>
#include <string>
#include <vector>

#include "../config/levels.hpp"
#include "kcd2s/integration/world_config.hpp"
#include "kcd2s/replication/world.hpp"

namespace kcd2s::local::maps {

// The server's maps from its level and its `maps` list (comma separated, may be empty): the level
// first, then the listed maps in their order, each once. Empty with a problem for a map the game
// does not have, or more maps than a dimension value can tell apart.
inline std::vector<std::string> server_maps(const std::string& level, const std::string& listed,
                                            std::string& problem) {
  std::vector<std::string> maps;
  const auto add = [&](std::string name) {
    while (!name.empty() && name.front() == ' ') {
      name.erase(name.begin());
    }
    while (!name.empty() && name.back() == ' ') {
      name.pop_back();
    }
    if (name.empty()) {
      return true;
    }
    if (!config::known_level(name)) {
      problem = "Unknown map \"" + name + "\" in maps: the server runs " + config::level_list();
      return false;
    }
    for (const std::string& known : maps) {
      if (known == name) {
        return true;
      }
    }
    maps.push_back(name);
    return true;
  };
  if (!add(level)) {
    return {};
  }
  std::size_t start = 0;
  while (start <= listed.size()) {
    const std::size_t comma = listed.find(',', start);
    const std::size_t end = comma == std::string::npos ? listed.size() : comma;
    if (!add(listed.substr(start, end - start))) {
      return {};
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  if (maps.size() > replication::kMaxMaps) {
    problem = "Too many maps: a server runs at most " + std::to_string(replication::kMaxMaps);
    return {};
  }
  return maps;
}

// The index of a map among the server's maps, or -1.
inline int map_index(const std::vector<std::string>& maps, const std::string& name) {
  for (std::size_t i = 0; i < maps.size(); ++i) {
    if (maps[i] == name) {
      return int(i);
    }
  }
  return -1;
}

// The world config a player on `map` gets: the server's own, with that map and the player's next
// revision (a client loads the world again only for a newer revision).
inline integration::WorldConfig world_on_map(const integration::WorldConfig& base,
                                             const std::string& map, std::uint32_t revision) {
  integration::WorldConfig config = base;
  config.map = map;
  config.revision = revision;
  return config;
}

}  // namespace kcd2s::local::maps
