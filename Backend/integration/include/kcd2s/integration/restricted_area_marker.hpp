#pragma once
#include <cstdint>
#include <vector>

// Whether players see the game's restricted-area marker, the warning that they are trespassing
// (the HUD's Trespassing clip; KCDS addition RestrictedAreaMarker.setEnabled).
//
//   RESTRICTED_AREA_MARKER : server -> a player's client, on the control channel (0):
//                   [tag][u8 shown]. Sent when it changes and, while it is off, to a player whose
//                   world got ready; a client shows it again whenever it leaves a world.
namespace kcd2s::integration {

inline constexpr std::uint8_t kRestrictedAreaMarkerTag = 57;

inline std::vector<std::uint8_t> encode_restricted_area_marker(bool shown) {
  return {kRestrictedAreaMarkerTag, std::uint8_t(shown ? 1 : 0)};
}

inline bool decode_restricted_area_marker(const std::vector<std::uint8_t>& p, bool& shown) {
  if (p.size() != 2 || p[0] != kRestrictedAreaMarkerTag || p[1] > 1) {
    return false;
  }
  shown = p[1] == 1;
  return true;
}

}  // namespace kcd2s::integration
