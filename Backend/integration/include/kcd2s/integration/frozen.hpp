#pragma once
#include <cstdint>
#include <vector>

// A frozen player (KCDS addition Player.frozen).
//
//   FROZEN : server -> a player's client, on the control channel (0): [tag][u8 frozen 0/1].
//            Sent when a resource freezes or frees the player and, while frozen, to a player whose
//            world got ready. While frozen the client lets only the player's looking around and
//            the menus through; the server puts the body back where it was frozen if it moves
//            anyway. A client frees the player whenever it leaves a world.
namespace kcd2s::integration {

inline constexpr std::uint8_t kFrozenTag = 162;

inline std::vector<std::uint8_t> encode_frozen(bool frozen) {
  return {kFrozenTag, std::uint8_t(frozen ? 1 : 0)};
}

inline bool decode_frozen(const std::vector<std::uint8_t>& p, bool& frozen) {
  if (p.size() != 2 || p[0] != kFrozenTag || p[1] > 1) {
    return false;
  }
  frozen = p[1] == 1;
  return true;
}

}  // namespace kcd2s::integration
