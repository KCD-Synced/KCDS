#pragma once
#include <cstdint>
#include <vector>

// Handcuffs on a player (KCDS addition Player.setCuffed).
//
//   CUFFS : server -> a player's client, on the control channel (0): [tag][u8 cuffed 0/1].
//           Sent when a resource cuffs or uncuffs the player and, while cuffed, to a player whose
//           world got ready. While cuffed the client lets only the player's walking and looking
//           around through; the pose itself is the player's animation (the server's order). A
//           client takes the cuffs off whenever it leaves a world.
namespace kcd2s::integration {

inline constexpr std::uint8_t kCuffsTag = 61;

inline std::vector<std::uint8_t> encode_cuffs(bool cuffed) {
  return {kCuffsTag, std::uint8_t(cuffed ? 1 : 0)};
}

inline bool decode_cuffs(const std::vector<std::uint8_t>& p, bool& cuffed) {
  if (p.size() != 2 || p[0] != kCuffsTag || p[1] > 1) {
    return false;
  }
  cuffed = p[1] == 1;
  return true;
}

}  // namespace kcd2s::integration
