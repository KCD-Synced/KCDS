#pragma once
#include <cstdint>
#include <vector>

// Whether players see the game's combat marker, the crossed swords under the compass while they
// fight (the HUD's Crime clip; KCDS addition CombatMarker.setEnabled).
//
//   COMBAT_MARKER : server -> a player's client, on the control channel (0): [tag][u8 shown]. Sent
//                   when it changes and, while it is off, to a player whose world got ready; a
//                   client shows it again whenever it leaves a world.
namespace kcd2s::integration {

inline constexpr std::uint8_t kCombatMarkerTag = 51;

inline std::vector<std::uint8_t> encode_combat_marker(bool shown) {
  return {kCombatMarkerTag, std::uint8_t(shown ? 1 : 0)};
}

inline bool decode_combat_marker(const std::vector<std::uint8_t>& p, bool& shown) {
  if (p.size() != 2 || p[0] != kCombatMarkerTag || p[1] > 1) {
    return false;
  }
  shown = p[1] == 1;
  return true;
}

}  // namespace kcd2s::integration
