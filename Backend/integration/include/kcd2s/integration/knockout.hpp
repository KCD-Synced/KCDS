#pragma once
#include <cstdint>
#include <vector>

#include "kcd2s/core/ids.hpp"

// Knock-out instead of death, on the control channel (0). Tags 1-7 are taken by
// control.hpp and world_config.hpp.
//   KNOCKED_OUT : the owner's player was knocked out by a lethal hit in this world revision
//                 (client -> server); the resources hear it as playerDied.
//   REVIVE      : a resource revived the player (server -> its owner's client).
//   KNOCKOUT_VISION : how much of the world the player sees while knocked out, in percent
//                 (server -> its owner's client): 0 is a black screen, more shows the
//                 world darkened to that percent.
namespace kcd2s::integration {
inline constexpr std::uint8_t kKnockedOutTag = 8;
inline constexpr std::uint8_t kReviveTag = 9;
inline constexpr std::uint8_t kKnockoutVisionTag = 10;
inline constexpr std::uint8_t kKnockoutVisionMax = 100;
// What a knocked-out player sees before the server sets a vision: the world from the ground,
// dimmed. 0 is black.
inline constexpr std::uint8_t kKnockoutVisionDefault = 40;

inline std::vector<std::uint8_t> encode_knocked_out(std::uint32_t revision) {
  std::vector<std::uint8_t> p{kKnockedOutTag};
  for (int i = 0; i < 4; ++i) {
    p.push_back(static_cast<std::uint8_t>(revision >> (8 * i)));
  }
  return p;
}

inline bool decode_knocked_out(const std::vector<std::uint8_t>& p, std::uint32_t& revision) {
  if (p.size() != 5 || p[0] != kKnockedOutTag) {
    return false;
  }
  revision = 0;
  for (int i = 0; i < 4; ++i) {
    revision |= std::uint32_t(p[1 + i]) << (8 * i);
  }
  return revision != 0;
}

inline std::vector<std::uint8_t> encode_revive(core::EntityId player) {
  std::vector<std::uint8_t> p{kReviveTag};
  for (int i = 0; i < 4; ++i) {
    p.push_back(static_cast<std::uint8_t>(player >> (8 * i)));
  }
  return p;
}

inline bool decode_revive(const std::vector<std::uint8_t>& p, core::EntityId& player) {
  if (p.size() != 5 || p[0] != kReviveTag) {
    return false;
  }
  std::uint32_t id = 0;
  for (int i = 0; i < 4; ++i) {
    id |= std::uint32_t(p[1 + i]) << (8 * i);
  }
  player = core::EntityId(id);
  return player != 0;
}

inline std::vector<std::uint8_t> encode_knockout_vision(std::uint8_t percent) {
  return {kKnockoutVisionTag, percent};
}

inline bool decode_knockout_vision(const std::vector<std::uint8_t>& p, std::uint8_t& percent) {
  if (p.size() != 2 || p[0] != kKnockoutVisionTag || p[1] > kKnockoutVisionMax) {
    return false;
  }
  percent = p[1];
  return true;
}
}  // namespace kcd2s::integration
