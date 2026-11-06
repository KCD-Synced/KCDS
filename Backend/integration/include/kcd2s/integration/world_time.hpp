#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "kcd2s/core/bits.hpp"

// The world clock: the server holds the day, the hour and how fast
// the time runs; every ready player's game shows the same time.
//
//   WORLD_TIME : server -> a ready player's client, on the control channel (0): the game's own
//                clock is set to `day` and `hour`, and runs at `timeScale` game seconds per real
//                second (0 freezes it). Sent when a script changes the clock, as a correction
//                about every minute while it runs, and when the player's world becomes ready.
//                Client/runtime/world/world_clock.hpp applies it.
namespace kcd2s::integration {

inline constexpr std::uint8_t kWorldTimeTag = 69;
// The largest day and time scale that travel (scripts run for years of game days).
inline constexpr std::uint32_t kMaxWorldDay = 1000000;
inline constexpr float kMaxWorldTimeScale = 10000.0f;

struct WorldTime {
  std::uint32_t day = 0;   // whole days since the start of the game's calendar
  float hour = 0.0f;       // 0 up to (not including) 24
  float timeScale = 0.0f;  // game seconds per real second; 0 is frozen

  friend bool operator==(const WorldTime& a, const WorldTime& b) {
    return a.day == b.day && a.hour == b.hour && a.timeScale == b.timeScale;
  }
};

namespace world_time_detail {

inline std::uint32_t float_bits(float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

inline float bits_float(std::uint32_t bits) {
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

inline bool valid(const WorldTime& time) {
  return time.day <= kMaxWorldDay && std::isfinite(time.hour) && time.hour >= 0.0f &&
         time.hour < 24.0f && std::isfinite(time.timeScale) && time.timeScale >= 0.0f &&
         time.timeScale <= kMaxWorldTimeScale;
}

}  // namespace world_time_detail

// WORLD_TIME: [tag][varuint day][u32 hour as float bits][u32 timeScale as float bits]; empty for
// a time that is out of range.
inline std::vector<std::uint8_t> encode_world_time(const WorldTime& time) {
  if (!world_time_detail::valid(time)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kWorldTimeTag, 8);
  w.write_varuint(time.day);
  w.write_bits(world_time_detail::float_bits(time.hour), 32);
  w.write_bits(world_time_detail::float_bits(time.timeScale), 32);
  return w.bytes();
}

inline bool decode_world_time(const std::vector<std::uint8_t>& p, WorldTime& time) {
  if (p.empty() || p[0] != kWorldTimeTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  const std::uint64_t day = r.read_varuint();
  WorldTime decoded;
  decoded.hour = world_time_detail::bits_float(std::uint32_t(r.read_bits(32)));
  decoded.timeScale = world_time_detail::bits_float(std::uint32_t(r.read_bits(32)));
  if (r.overrun() || day > kMaxWorldDay) {
    return false;
  }
  decoded.day = std::uint32_t(day);
  if (!world_time_detail::valid(decoded)) {
    return false;
  }
  time = decoded;
  return true;
}

}  // namespace kcd2s::integration
