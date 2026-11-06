#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "kcd2s/core/bits.hpp"

// The level's graves. A grave is an entity of the game's class
// "Hole" (Scripts/Entities/WH/Others/Hole.lua): its script's nDepth is how far it is dug (0 full,
// 1 dug out), and the game hides the entity's soil once it reaches 1. Digging is the game's own
// minigame on the player's own game, so a grave one player dug stayed full for everyone else.
// A grave is named by where it lies: its position in whole centimetres, the same on every client.
//
//   GRAVE : on the control channel (0), [tag][kind u8][x i64][y i64][z i64][dug u8], the position
//           as zig-zag varints:
//     kReport  client -> server: a grave the client's level has, with the state its game shows,
//              once after entering a world for each grave it finds.
//     kDug     client -> server: the local player dug this grave out (dug is 1).
//     kSet     server -> client: the grave's state; the client applies it to its game's grave.
//     kDigging client -> server -> every other client: the local player's digging opened the
//              grave's soil (dug 1) or stopped short and closed it again (dug 0). The server relays
//              it at once and keeps nothing: the grave's state changes with kDug only.
//
// Peers from before 0.3.4.1 ignore the tag.
namespace kcd2s::integration {

inline constexpr std::uint8_t kGraveTag = 160;

// How many graves one level may have, and how many GRAVE messages the server takes from one
// player in a second.
inline constexpr std::size_t kMaxGraves = 1024;
inline constexpr unsigned kMaxGraveMessagesPerSecond = 64;
// The farthest (metres) a player may stand from a grave the server lets them report dug.
inline constexpr float kGraveDigReach = 6.0f;
// A grave position's bound in centimetres (the game's levels are far smaller).
inline constexpr std::int64_t kMaxGraveCentimetres = 10000000;

enum class GraveKind : std::uint8_t {
  Report = 0,
  Dug = 1,
  Set = 2,
  Digging = 3,
};

struct Grave {
  GraveKind kind = GraveKind::Report;
  std::int64_t position[3] = {0, 0, 0};  // centimetres
  bool dug = false;

  bool operator==(const Grave& other) const {
    return kind == other.kind && position[0] == other.position[0] &&
           position[1] == other.position[1] && position[2] == other.position[2] && dug == other.dug;
  }
};

namespace graves_detail {

inline bool valid(const Grave& grave) {
  if (grave.kind != GraveKind::Report && grave.kind != GraveKind::Dug &&
      grave.kind != GraveKind::Set && grave.kind != GraveKind::Digging) {
    return false;
  }
  for (const std::int64_t axis : grave.position) {
    if (axis < -kMaxGraveCentimetres || axis > kMaxGraveCentimetres) {
      return false;
    }
  }
  return grave.kind != GraveKind::Dug || grave.dug;
}

}  // namespace graves_detail

// A grave's id as scripts see it: its position in centimetres, "x,y,z".
inline std::string grave_id(const std::int64_t position[3]) {
  return std::to_string(position[0]) + "," + std::to_string(position[1]) + "," +
         std::to_string(position[2]);
}

// The position of a grave id; false for anything but three whole numbers within bounds.
inline bool parse_grave_id(const std::string& id, std::int64_t position[3]) {
  std::size_t at = 0;
  for (int axis = 0; axis < 3; ++axis) {
    const bool negative = at < id.size() && id[at] == '-';
    if (negative) {
      ++at;
    }
    const std::size_t start = at;
    std::int64_t value = 0;
    while (at < id.size() && id[at] >= '0' && id[at] <= '9' && at - start < 9) {
      value = value * 10 + (id[at] - '0');
      ++at;
    }
    if (at == start || value > kMaxGraveCentimetres) {
      return false;
    }
    position[axis] = negative ? -value : value;
    if (axis < 2) {
      if (at >= id.size() || id[at] != ',') {
        return false;
      }
      ++at;
    }
  }
  return at == id.size();
}

// Empty for a grave that cannot travel.
inline std::vector<std::uint8_t> encode_grave(const Grave& grave) {
  if (!graves_detail::valid(grave)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kGraveTag, 8);
  w.write_bits(std::uint8_t(grave.kind), 8);
  for (const std::int64_t axis : grave.position) {
    w.write_varint(axis);
  }
  w.write_bits(grave.dug ? 1u : 0u, 8);
  return w.bytes();
}

inline bool decode_grave(const std::vector<std::uint8_t>& p, Grave& grave) {
  if (p.empty() || p[0] != kGraveTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  Grave decoded;
  decoded.kind = GraveKind(r.read_bits(8));
  for (std::int64_t& axis : decoded.position) {
    axis = r.read_varint();
  }
  const std::uint64_t dug = r.read_bits(8);
  if (r.overrun() || dug > 1 || r.bits_remaining() >= 8) {
    return false;
  }
  decoded.dug = dug == 1;
  if (!graves_detail::valid(decoded)) {
    return false;
  }
  grave = decoded;
  return true;
}

}  // namespace kcd2s::integration
