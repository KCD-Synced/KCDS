#pragma once
#include <cstdint>
#include <vector>

// A player's whistle for their horse, heard by the players near them (KCDS addition: the event
// playerWhistle and Player.whistle).
//
//   WHISTLE : client -> server, on the control channel (0): [tag]. The local player whistled
//             (the companion key, as the server allows it, or the server's Player.whistle).
//             At most one a second counts.
//   WHISTLE : server -> a player's client: [tag][u32 whistler]. 0: whistle (Player.whistle, the
//             game's own whistle, which calls the horse); a player id: that player whistled near
//             you, so their body here plays the whistle.
namespace kcd2s::integration {

inline constexpr std::uint8_t kWhistleTag = 144;

inline std::vector<std::uint8_t> encode_whistle_report() {
  return {kWhistleTag};
}

inline bool decode_whistle_report(const std::vector<std::uint8_t>& p) {
  return p.size() == 1 && p[0] == kWhistleTag;
}

// The whistler's id, little-endian.
inline std::vector<std::uint8_t> encode_whistle(std::uint32_t whistler) {
  std::vector<std::uint8_t> out = {kWhistleTag};
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(std::uint8_t(whistler >> shift));
  }
  return out;
}

inline bool decode_whistle(const std::vector<std::uint8_t>& p, std::uint32_t& whistler) {
  if (p.size() != 5 || p[0] != kWhistleTag) {
    return false;
  }
  whistler = 0;
  for (int index = 0; index < 4; ++index) {
    whistler |= std::uint32_t(p[1 + index]) << (8 * index);
  }
  return true;
}

}  // namespace kcd2s::integration
