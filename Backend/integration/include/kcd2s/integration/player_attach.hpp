#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

// One player attached to another (KCDS addition Player.attachTo).
//
//   PLAYER_ATTACH : server -> every client, on the control channel (0):
//                   [tag][u8 flags][u32 player][u32 target, 0 detached][f32 right][f32 forward]
//                   [f32 up][f32 yaw degrees]. Sent when a resource attaches or detaches a player
//                   and, for each attached player, whenever a player's world gets ready. The
//                   attached player's own client keeps its body at that offset from the target's
//                   and lets only looking around and the menus through; every client shows the
//                   attached body there, from where it shows the target. A client forgets the
//                   orders whenever it leaves a world.
namespace kcd2s::integration {

inline constexpr std::uint8_t kPlayerAttachTag = 165;
// The order is about the player of the client that gets it.
inline constexpr std::uint8_t kAttachIsLocalPlayer = 1;
// The target is the player of the client that gets it.
inline constexpr std::uint8_t kAttachToLocalPlayer = 2;
// The body faces the way the target faces and its player cannot look around (look: 'follow').
inline constexpr std::uint8_t kAttachFollowLook = 4;

struct PlayerAttach {
  std::uint8_t flags = 0;
  std::uint32_t player = 0;
  std::uint32_t target = 0;
  float right = 0.0f;
  float forward = 0.0f;
  float up = 0.0f;
  float yaw = 0.0f;
};

inline std::vector<std::uint8_t> encode_player_attach(const PlayerAttach& attach) {
  std::vector<std::uint8_t> bytes(26);
  bytes[0] = kPlayerAttachTag;
  bytes[1] = attach.flags;
  std::memcpy(&bytes[2], &attach.player, 4);
  std::memcpy(&bytes[6], &attach.target, 4);
  std::memcpy(&bytes[10], &attach.right, 4);
  std::memcpy(&bytes[14], &attach.forward, 4);
  std::memcpy(&bytes[18], &attach.up, 4);
  std::memcpy(&bytes[22], &attach.yaw, 4);
  return bytes;
}

inline bool decode_player_attach(const std::vector<std::uint8_t>& p, PlayerAttach& attach) {
  if (p.size() != 26 || p[0] != kPlayerAttachTag) {
    return false;
  }
  attach.flags = p[1];
  std::memcpy(&attach.player, &p[2], 4);
  std::memcpy(&attach.target, &p[6], 4);
  std::memcpy(&attach.right, &p[10], 4);
  std::memcpy(&attach.forward, &p[14], 4);
  std::memcpy(&attach.up, &p[18], 4);
  std::memcpy(&attach.yaw, &p[22], 4);
  return true;
}

}  // namespace kcd2s::integration
