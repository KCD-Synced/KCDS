#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace kcd2s::integration {
// Server-owned world selection. Never interpret map as a console command or filesystem path.
struct WorldConfig {
  std::uint32_t revision = 1;
  std::string map = "trosecko";
  bool debug = true;
  bool quests = false;
  // The game client lifts the fog of war and marks every place discovered (server.json
  // revealMap, on by default); off, the fog and the discoveries are the game's own.
  bool reveal_map = true;
  // The level's own people live in every game (server.json levelPeople, off by default): each
  // game lets the level spawn them, and the server gives each one to a player's game to run
  // (issue 016). Off, the game's own people stay out and the level's people are script NPCs.
  bool level_people = false;
};
// The world config's flags byte: debug without quests, and the map left to the game.
inline constexpr std::uint8_t kWorldDebug = 1;
inline constexpr std::uint8_t kWorldMapUnrevealed = 2;
inline constexpr std::uint8_t kWorldLevelPeople = 4;
// A World Architect custom map (Client/runtime/maps/custom_maps.hpp): kcds_map_ and 1 to 32
// lower-case letters, digits or _.
inline bool custom_map_name(const std::string& map) {
  constexpr std::size_t kPrefix = 9;
  if (map.size() <= kPrefix || map.size() > kPrefix + 32 ||
      map.compare(0, kPrefix, "kcds_map_") != 0)
    return false;
  for (std::size_t i = kPrefix; i < map.size(); ++i) {
    const char c = map[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
      return false;
  }
  return true;
}
// The Kingdom Come: Deliverance map, which every player's game builds from its own copy of that
// game (Client/runtime/kcd1).
inline bool kcd1_map_name(const std::string& map) {
  return map == "kcd1_rataje";
}
inline bool valid_world_config(const WorldConfig& c) {
  return c.revision != 0 && c.debug && !c.quests &&
         (c.map == "trosecko" || c.map == "kutnohorsko" || c.map == "klaster" ||
          kcd1_map_name(c.map) || custom_map_name(c.map));
}
inline std::vector<std::uint8_t> encode_world_config(const WorldConfig& c) {
  if (!valid_world_config(c))
    return {};
  std::vector<std::uint8_t> p{3};
  for (int i = 0; i < 4; ++i)
    p.push_back(static_cast<std::uint8_t>(c.revision >> (8 * i)));
  // Debug without quests; other combinations deliberately unsupported.
  p.push_back(std::uint8_t(kWorldDebug | (c.reveal_map ? 0 : kWorldMapUnrevealed) |
                           (c.level_people ? kWorldLevelPeople : 0)));
  p.push_back(static_cast<std::uint8_t>(c.map.size()));
  p.insert(p.end(), c.map.begin(), c.map.end());
  return p;
}
inline bool decode_world_config(const std::vector<std::uint8_t>& p, WorldConfig& out) {
  if (p.size() < 7 || p[0] != 3 ||
      (p[5] & ~(kWorldMapUnrevealed | kWorldLevelPeople)) != kWorldDebug ||
      p.size() != std::size_t(7 + p[6]))
    return false;
  WorldConfig c;
  c.reveal_map = (p[5] & kWorldMapUnrevealed) == 0;
  c.level_people = (p[5] & kWorldLevelPeople) != 0;
  c.revision = 0;
  for (int i = 0; i < 4; ++i)
    c.revision |= std::uint32_t(p[1 + i]) << (8 * i);
  c.map.assign(p.begin() + 7, p.end());
  if (!valid_world_config(c))
    return false;
  out = std::move(c);
  return true;
}
inline std::vector<std::uint8_t> encode_world_ready(std::uint32_t revision) {
  std::vector<std::uint8_t> p{4};
  for (int i = 0; i < 4; ++i)
    p.push_back(static_cast<std::uint8_t>(revision >> (8 * i)));
  return p;
}
inline bool decode_world_ready(const std::vector<std::uint8_t>& p, std::uint32_t& revision) {
  if (p.size() != 5 || p[0] != 4)
    return false;
  revision = 0;
  for (int i = 0; i < 4; ++i)
    revision |= std::uint32_t(p[1 + i]) << (8 * i);
  return revision != 0;
}
// Sent once per world revision after world-ready, when the game took its own loading screen down:
// the player can play (playerReady).
inline std::vector<std::uint8_t> encode_gameplay_ready(std::uint32_t revision) {
  std::vector<std::uint8_t> p{7};
  for (int i = 0; i < 4; ++i)
    p.push_back(static_cast<std::uint8_t>(revision >> (8 * i)));
  return p;
}
inline bool decode_gameplay_ready(const std::vector<std::uint8_t>& p, std::uint32_t& revision) {
  if (p.size() != 5 || p[0] != 7)
    return false;
  revision = 0;
  for (int i = 0; i < 4; ++i)
    revision |= std::uint32_t(p[1 + i]) << (8 * i);
  return revision != 0;
}
}  // namespace kcd2s::integration
