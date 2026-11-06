#pragma once
#include <cstdint>
#include <vector>

// A server's limit on how one player moves (Player.setPace), on the control
// channel (0); tags 1-15 are taken by control.hpp, world_config.hpp, knockout.hpp, appearance.hpp,
// player_identity.hpp and player_stats.hpp.
//   MOVEMENT_MODE : server -> the player's own client, the whole rule: walk by default (a
//                   preference the player's own key still overrides) and walk enforced (no running
//                   or sprinting at all). Each message replaces the previous rule; the client keeps
//                   it for the rest of the connection.
namespace kcd2s::integration {
inline constexpr std::uint8_t kMovementModeTag = 16;

struct MovementMode {
  bool walk_by_default = false;
  bool walk_enforced = false;

  friend bool operator==(const MovementMode& a, const MovementMode& b) {
    return a.walk_by_default == b.walk_by_default && a.walk_enforced == b.walk_enforced;
  }
};

inline constexpr std::uint8_t kWalkByDefaultBit = 1;
inline constexpr std::uint8_t kWalkEnforcedBit = 2;

inline std::vector<std::uint8_t> encode_movement_mode(const MovementMode& mode) {
  std::uint8_t flags = 0;
  if (mode.walk_by_default) {
    flags |= kWalkByDefaultBit;
  }
  if (mode.walk_enforced) {
    flags |= kWalkEnforcedBit;
  }
  return {kMovementModeTag, flags};
}

inline bool decode_movement_mode(const std::vector<std::uint8_t>& p, MovementMode& mode) {
  if (p.size() != 2 || p[0] != kMovementModeTag ||
      (p[1] & ~(kWalkByDefaultBit | kWalkEnforcedBit)) != 0) {
    return false;
  }
  mode.walk_by_default = (p[1] & kWalkByDefaultBit) != 0;
  mode.walk_enforced = (p[1] & kWalkEnforcedBit) != 0;
  return true;
}

}  // namespace kcd2s::integration
