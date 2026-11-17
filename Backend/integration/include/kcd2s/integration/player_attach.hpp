#pragma once
#include <cstddef>
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
//                   With kAttachMoving the order goes on with [f32 from right][f32 from forward]
//                   [f32 from up][f32 from yaw degrees][u16 milliseconds]: the body starts at that
//                   offset and moves to the order's own in that time, as a clip carries it (a
//                   horse passenger's climb on and off).
namespace kcd2s::integration {

inline constexpr std::uint8_t kPlayerAttachTag = 165;
// The order is about the player of the client that gets it.
inline constexpr std::uint8_t kAttachIsLocalPlayer = 1;
// The target is the player of the client that gets it.
inline constexpr std::uint8_t kAttachToLocalPlayer = 2;
// The body faces the way the target faces and its player cannot look around (look: 'follow').
inline constexpr std::uint8_t kAttachFollowLook = 4;
// The target is a body by its network id (a horse), not a player.
inline constexpr std::uint8_t kAttachToBody = 8;
// The body moves from the order's `from` offset to its own (the longer form).
inline constexpr std::uint8_t kAttachMoving = 16;

inline constexpr std::size_t kPlayerAttachSize = 26;
inline constexpr std::size_t kPlayerAttachMovingSize = 44;

struct PlayerAttach {
  std::uint8_t flags = 0;
  std::uint32_t player = 0;
  std::uint32_t target = 0;
  float right = 0.0f;
  float forward = 0.0f;
  float up = 0.0f;
  float yaw = 0.0f;
  // Where the body starts with kAttachMoving, and how long it takes to the offset above.
  float from_right = 0.0f;
  float from_forward = 0.0f;
  float from_up = 0.0f;
  float from_yaw = 0.0f;
  std::uint16_t moving_ms = 0;
};

inline std::vector<std::uint8_t> encode_player_attach(const PlayerAttach& attach) {
  const bool moving = (attach.flags & kAttachMoving) != 0;
  std::vector<std::uint8_t> bytes(moving ? kPlayerAttachMovingSize : kPlayerAttachSize);
  bytes[0] = kPlayerAttachTag;
  bytes[1] = attach.flags;
  std::memcpy(&bytes[2], &attach.player, 4);
  std::memcpy(&bytes[6], &attach.target, 4);
  std::memcpy(&bytes[10], &attach.right, 4);
  std::memcpy(&bytes[14], &attach.forward, 4);
  std::memcpy(&bytes[18], &attach.up, 4);
  std::memcpy(&bytes[22], &attach.yaw, 4);
  if (moving) {
    std::memcpy(&bytes[26], &attach.from_right, 4);
    std::memcpy(&bytes[30], &attach.from_forward, 4);
    std::memcpy(&bytes[34], &attach.from_up, 4);
    std::memcpy(&bytes[38], &attach.from_yaw, 4);
    std::memcpy(&bytes[42], &attach.moving_ms, 2);
  }
  return bytes;
}

inline bool decode_player_attach(const std::vector<std::uint8_t>& p, PlayerAttach& attach) {
  if (p.size() < kPlayerAttachSize || p[0] != kPlayerAttachTag) {
    return false;
  }
  const bool moving = (p[1] & kAttachMoving) != 0;
  if (p.size() != (moving ? kPlayerAttachMovingSize : kPlayerAttachSize)) {
    return false;
  }
  attach = PlayerAttach{};
  attach.flags = p[1];
  std::memcpy(&attach.player, &p[2], 4);
  std::memcpy(&attach.target, &p[6], 4);
  std::memcpy(&attach.right, &p[10], 4);
  std::memcpy(&attach.forward, &p[14], 4);
  std::memcpy(&attach.up, &p[18], 4);
  std::memcpy(&attach.yaw, &p[22], 4);
  if (moving) {
    std::memcpy(&attach.from_right, &p[26], 4);
    std::memcpy(&attach.from_forward, &p[30], 4);
    std::memcpy(&attach.from_up, &p[34], 4);
    std::memcpy(&attach.from_yaw, &p[38], 4);
    std::memcpy(&attach.moving_ms, &p[42], 2);
  }
  return true;
}

// The offset a moving order's body has `elapsed_ms` after the order came: from its `from` offset
// to its own, eased in and out as a climb starts and ends; the order's own offset once there.
inline PlayerAttach attach_offset_at(const PlayerAttach& attach, double elapsed_ms) {
  PlayerAttach at = attach;
  if ((attach.flags & kAttachMoving) == 0 || attach.moving_ms == 0 ||
      elapsed_ms >= double(attach.moving_ms)) {
    return at;
  }
  double t = elapsed_ms <= 0.0 ? 0.0 : elapsed_ms / double(attach.moving_ms);
  t = t * t * (3.0 - 2.0 * t);
  const auto blend = [t](float from, float to) {
    return static_cast<float>(double(from) + (double(to) - double(from)) * t);
  };
  at.right = blend(attach.from_right, attach.right);
  at.forward = blend(attach.from_forward, attach.forward);
  at.up = blend(attach.from_up, attach.up);
  at.yaw = blend(attach.from_yaw, attach.yaw);
  return at;
}

}  // namespace kcd2s::integration
