#pragma once
#include <cstdint>
#include <vector>

// Whether a player may open the game's photo mode, its free camera on F1 (KCDS addition
// Freecam.setEnabled).
//
//   FREECAM : server -> a player's client, on the control channel (0): [tag][u8 allowed]. Sent when
//             it changes and, while it is off, to a player whose world got ready; a client allows
//             it again whenever it leaves a world.
namespace kcd2s::integration {

inline constexpr std::uint8_t kFreecamTag = 50;

inline std::vector<std::uint8_t> encode_freecam(bool allowed) {
  return {kFreecamTag, std::uint8_t(allowed ? 1 : 0)};
}

inline bool decode_freecam(const std::vector<std::uint8_t>& p, bool& allowed) {
  if (p.size() != 2 || p[0] != kFreecamTag || p[1] > 1) {
    return false;
  }
  allowed = p[1] == 1;
  return true;
}

}  // namespace kcd2s::integration
