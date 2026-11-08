#pragma once
#include <cstdint>
#include <vector>

// A player held out of the world (KCDS addition Player.setLobby), while a server makes their
// character or logs them in.
//
//   LOBBY : server -> a player's client, on the control channel (0): [tag][u8 held 0/1].
//           Sent when a resource holds or frees the player and, while held, to a player whose
//           world got ready. While held their game makes no sound of the world and a server's
//           loading page stays up; the server keeps them out of everyone's sight and voice. A
//           client lets the player go whenever it leaves a world.
namespace kcd2s::integration {

inline constexpr std::uint8_t kLobbyTag = 167;

inline std::vector<std::uint8_t> encode_lobby(bool held) {
  return {kLobbyTag, std::uint8_t(held ? 1 : 0)};
}

inline bool decode_lobby(const std::vector<std::uint8_t>& p, bool& held) {
  if (p.size() != 2 || p[0] != kLobbyTag || p[1] > 1) {
    return false;
  }
  held = p[1] == 1;
  return true;
}

}  // namespace kcd2s::integration
