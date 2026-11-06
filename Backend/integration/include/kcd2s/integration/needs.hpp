#pragma once
#include <cstdint>
#include <vector>

// Whether the game's sleep (exhaust) and food (hunger) needs run for the players (KCDS addition
// Needs.setEnabled).
//
//   NEEDS : server -> a player's client, on the control channel (0): [tag][u8 enabled]. Sent when
//           it changes and, while needs are off, to a player whose world got ready; a client lets
//           its needs run again whenever it leaves a world.
namespace kcd2s::integration {

inline constexpr std::uint8_t kNeedsTag = 53;

inline std::vector<std::uint8_t> encode_needs(bool enabled) {
  return {kNeedsTag, std::uint8_t(enabled ? 1 : 0)};
}

inline bool decode_needs(const std::vector<std::uint8_t>& p, bool& enabled) {
  if (p.size() != 2 || p[0] != kNeedsTag || p[1] > 1) {
    return false;
  }
  enabled = p[1] == 1;
  return true;
}

}  // namespace kcd2s::integration
