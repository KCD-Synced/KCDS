#pragma once
#include <cstdint>
#include <vector>

// A resource puts a player in a horse's saddle (player.mount(horse)), or takes them
// out of it (player.dismount, a mount horseMounting refused).
//
//   MOUNT_ORDER : server -> the player's client, on the control channel (0):
//                 [u8 kMountOrderTag][u32 horse][u8 instant 0/1], little-endian. The client mounts
//                 its player on the horse's body with the game's own mount (the get-on, or the
//                 instant seat), as when the player mounts by themselves, so the seat is reported
//                 back like any other mount (horseMounting, horseMount). A client whose player
//                 does not stand near the horse's body, or has none, does nothing.
//   DISMOUNT_ORDER : server -> the player's client, on the control channel (0):
//                 [u8 kDismountOrderTag]. The client takes its player out of the saddle with the
//                 game's own dismount, which is reported like any other.
//                 [u8 kDismountOrderTag][u8 1]: the horse rears and throws them off instead, the
//                 game's own (Horse.buckRider).
namespace kcd2s::integration {

inline constexpr std::uint8_t kMountOrderTag = 149;
inline constexpr std::size_t kMountOrderBytes = 6;
inline constexpr std::uint8_t kDismountOrderTag = 150;

struct MountOrder {
  std::uint32_t horse = 0;
  bool instant = false;
};

inline std::vector<std::uint8_t> encode_mount_order(const MountOrder& order) {
  if (order.horse == 0) {
    return {};
  }
  std::vector<std::uint8_t> out = {kMountOrderTag};
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(std::uint8_t((order.horse >> shift) & 0xff));
  }
  out.push_back(std::uint8_t(order.instant ? 1 : 0));
  return out;
}

inline bool decode_mount_order(const std::vector<std::uint8_t>& in, MountOrder& order) {
  if (in.size() != kMountOrderBytes || in[0] != kMountOrderTag || in[5] > 1) {
    return false;
  }
  std::uint32_t horse = 0;
  for (int k = 0; k < 4; ++k) {
    horse |= std::uint32_t(in[1 + k]) << (8 * k);
  }
  if (horse == 0) {
    return false;
  }
  order.horse = horse;
  order.instant = in[5] == 1;
  return true;
}

inline std::vector<std::uint8_t> encode_dismount_order() {
  return {kDismountOrderTag};
}

inline bool decode_dismount_order(const std::vector<std::uint8_t>& in) {
  return in.size() == 1 && in[0] == kDismountOrderTag;
}

inline std::vector<std::uint8_t> encode_rear_order() {
  return {kDismountOrderTag, 1};
}

inline bool decode_rear_order(const std::vector<std::uint8_t>& in) {
  return in.size() == 2 && in[0] == kDismountOrderTag && in[1] == 1;
}

// [u8 kDismountOrderTag][u8 2][u32 attacker][u32 rider]: Horse.dragRider, to both
// players' clients. Each asks the game's own pull-down of the attacker's body on the rider's.
struct PullDownOrder {
  std::uint32_t attacker = 0;
  std::uint32_t rider = 0;
};

inline std::vector<std::uint8_t> encode_pull_down_order(const PullDownOrder& order) {
  std::vector<std::uint8_t> out = {kDismountOrderTag, 2};
  for (const std::uint32_t value : {order.attacker, order.rider}) {
    for (int shift = 0; shift < 32; shift += 8) {
      out.push_back(std::uint8_t((value >> shift) & 0xff));
    }
  }
  return out;
}

inline bool decode_pull_down_order(const std::vector<std::uint8_t>& in, PullDownOrder& order) {
  if (in.size() != 10 || in[0] != kDismountOrderTag || in[1] != 2) {
    return false;
  }
  std::uint32_t parts[2] = {0, 0};
  for (int part = 0; part < 2; ++part) {
    for (int k = 0; k < 4; ++k) {
      parts[part] |= std::uint32_t(in[2 + part * 4 + k]) << (8 * k);
    }
  }
  order.attacker = parts[0];
  order.rider = parts[1];
  return order.attacker != 0 && order.rider != 0;
}

}  // namespace kcd2s::integration
