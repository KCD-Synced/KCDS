#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// Where a player's fast travel goes, for a server that runs more than one map. The game's map has
// points that lead to another level (the travel to Kuttenberg or back to Trosky); the server
// decides whether a travel to one takes the player on to the other map
// (maps/level_travel.cjs). The client only tells where a travel it began goes.
//
//   LEVEL_TRAVEL : client -> server, on the control channel (0): [tag][f32 x][f32 y][f32 z].
//                  The local player just began a fast travel whose route ends there. At most one
//                  a second counts.
namespace kcd2s::integration {

inline constexpr std::uint8_t kLevelTravelTag = 166;

struct TravelTarget {
  float x = 0;
  float y = 0;
  float z = 0;
};

inline void put_float(std::vector<std::uint8_t>& out, float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof bits);
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(std::uint8_t(bits >> shift));
  }
}

inline float take_float(const std::vector<std::uint8_t>& p, std::size_t at) {
  std::uint32_t bits = 0;
  for (int index = 0; index < 4; ++index) {
    bits |= std::uint32_t(p[at + index]) << (8 * index);
  }
  float value = 0;
  std::memcpy(&value, &bits, sizeof value);
  return value;
}

inline std::vector<std::uint8_t> encode_travel_target(const TravelTarget& target) {
  std::vector<std::uint8_t> out = {kLevelTravelTag};
  put_float(out, target.x);
  put_float(out, target.y);
  put_float(out, target.z);
  return out;
}

// False for another message, or a place that is not a finite point.
inline bool decode_travel_target(const std::vector<std::uint8_t>& p, TravelTarget& target) {
  if (p.size() != 13 || p[0] != kLevelTravelTag) {
    return false;
  }
  target.x = take_float(p, 1);
  target.y = take_float(p, 5);
  target.z = take_float(p, 9);
  return std::isfinite(target.x) && std::isfinite(target.y) && std::isfinite(target.z);
}

}  // namespace kcd2s::integration
