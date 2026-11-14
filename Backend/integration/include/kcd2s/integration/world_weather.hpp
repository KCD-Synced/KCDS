#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "kcd2s/core/bits.hpp"

// The world weather: the server picks the weather preset, how long
// the sky takes to change to it, the rain and the wind; every ready player's game shows them.
//
//   WORLD_WEATHER : server -> a ready player's client, on the control channel (0): the game
//                   changes its weather to `preset` over `remaining` game seconds (0 at once),
//                   and the wind blows as `wind`. Sent when a script changes the preset, the
//                   rain or the wind, and when the player's world becomes ready (then
//                   `remaining` is what is left of a blend in progress). With it goes the snow
//                   (World.setSnow): snow on surfaces, frost, freezing and falling flakes, which
//                   Client/runtime/winter/snow_stage.hpp draws.
//                   Client/runtime/world/world_weather.hpp applies it.
namespace kcd2s::integration {

inline constexpr std::uint8_t kWorldWeatherTag = 70;
// The longest preset name that travels (the server's preset names are 64 characters at most).
inline constexpr std::size_t kMaxWeatherPresetLength = 64;
// The largest blend (game seconds), wind (metres per second) and rain that travel.
inline constexpr float kMaxWeatherBlendSeconds = 86400.0f;
inline constexpr float kMaxWeatherWind = 1000.0f;
// Snow on surfaces (0 none, 1 a light cover, up to 10 deep), frost and freezing (0 to 1), and the
// falling flakes around the camera (0 none).
inline constexpr float kMaxSnowAmount = 10.0f;
inline constexpr std::uint32_t kMaxSnowFlakes = 10000;
// The flakes' size, from a tenth of the game's own to five times it.
inline constexpr float kMinSnowFlakeSize = 0.1f;
inline constexpr float kMaxSnowFlakeSize = 5.0f;

struct WorldWeather {
  std::string preset;      // the weather the sky changes to
  std::string previous;    // the weather it changes from
  float seconds = 0.0f;    // length of the blend, game seconds
  float remaining = 0.0f;  // what is left of the blend, game seconds
  float rainStrength = 0.0f;
  float rainLevel = 0.0f;
  float windX = 0.0f;
  float windY = 0.0f;
  float windZ = 0.0f;
  // Snow (World.setSnow): on surfaces, frost, freezing and the falling flakes.
  float snowAmount = 0.0f;
  float snowFrost = 0.0f;
  float snowFreezing = 0.0f;
  std::uint32_t snowFlakes = 0;
  // The flakes' size (1 the game's own) and the storm that drives them (0 calm, 1 a blizzard).
  float snowFlakeSize = 1.0f;
  float snowStorm = 0.0f;
  // Fog over the weather's own (World.setFog): 0 the weather's, up to 1, where little more than
  // arm's length is seen.
  float fog = 0.0f;

  friend bool operator==(const WorldWeather& a, const WorldWeather& b) {
    return a.preset == b.preset && a.previous == b.previous && a.seconds == b.seconds &&
           a.remaining == b.remaining && a.rainStrength == b.rainStrength &&
           a.rainLevel == b.rainLevel && a.windX == b.windX && a.windY == b.windY &&
           a.windZ == b.windZ && a.snowAmount == b.snowAmount && a.snowFrost == b.snowFrost &&
           a.snowFreezing == b.snowFreezing && a.snowFlakes == b.snowFlakes &&
           a.snowFlakeSize == b.snowFlakeSize && a.snowStorm == b.snowStorm && a.fog == b.fog;
  }
};

namespace world_weather_detail {

inline void write_float(core::BitWriter& w, float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  w.write_bits(bits, 32);
}

inline float read_float(core::BitReader& r) {
  const std::uint32_t bits = std::uint32_t(r.read_bits(32));
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

inline void write_text(core::BitWriter& w, const std::string& text) {
  w.write_varuint(text.size());
  w.write_byte_run(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

inline bool read_text(core::BitReader& r, std::string& text) {
  const std::uint64_t length = r.read_varuint();
  if (r.overrun() || length > kMaxWeatherPresetLength) {
    return false;
  }
  text.assign(std::size_t(length), '\0');
  r.read_byte_run(reinterpret_cast<std::uint8_t*>(text.data()), text.size());
  return !r.overrun();
}

inline bool in_range(float value, float largest) {
  return std::isfinite(value) && value >= 0.0f && value <= largest;
}

inline bool valid(const WorldWeather& weather) {
  const bool names = !weather.preset.empty() && weather.preset.size() <= kMaxWeatherPresetLength &&
                     weather.previous.size() <= kMaxWeatherPresetLength;
  const bool wind = std::isfinite(weather.windX) && std::isfinite(weather.windY) &&
                    std::isfinite(weather.windZ) && std::fabs(weather.windX) <= kMaxWeatherWind &&
                    std::fabs(weather.windY) <= kMaxWeatherWind &&
                    std::fabs(weather.windZ) <= kMaxWeatherWind;
  const bool snow = in_range(weather.snowAmount, kMaxSnowAmount) &&
                    in_range(weather.snowFrost, 1.0f) && in_range(weather.snowFreezing, 1.0f) &&
                    weather.snowFlakes <= kMaxSnowFlakes &&
                    in_range(weather.snowFlakeSize, kMaxSnowFlakeSize) &&
                    weather.snowFlakeSize >= kMinSnowFlakeSize &&
                    in_range(weather.snowStorm, 1.0f) && in_range(weather.fog, 1.0f);
  return names && wind && snow && in_range(weather.seconds, kMaxWeatherBlendSeconds) &&
         in_range(weather.remaining, kMaxWeatherBlendSeconds) &&
         in_range(weather.rainStrength, 1.0f) && in_range(weather.rainLevel, 1.0f);
}

}  // namespace world_weather_detail

// WORLD_WEATHER: [tag][preset][previous] (each a varuint length and the characters), then seconds,
// remaining, rainStrength, rainLevel, windX, windY, windZ, snowAmount, snowFrost, snowFreezing as
// u32 float bits and snowFlakes as a varuint, then fog, snowFlakeSize and snowStorm as u32 float
// bits (after the rest, so an older client reads what it knows and leaves them); empty for a
// weather that is out of range. A message without the last three has the defaults.
inline std::vector<std::uint8_t> encode_world_weather(const WorldWeather& weather) {
  if (!world_weather_detail::valid(weather)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kWorldWeatherTag, 8);
  world_weather_detail::write_text(w, weather.preset);
  world_weather_detail::write_text(w, weather.previous);
  world_weather_detail::write_float(w, weather.seconds);
  world_weather_detail::write_float(w, weather.remaining);
  world_weather_detail::write_float(w, weather.rainStrength);
  world_weather_detail::write_float(w, weather.rainLevel);
  world_weather_detail::write_float(w, weather.windX);
  world_weather_detail::write_float(w, weather.windY);
  world_weather_detail::write_float(w, weather.windZ);
  world_weather_detail::write_float(w, weather.snowAmount);
  world_weather_detail::write_float(w, weather.snowFrost);
  world_weather_detail::write_float(w, weather.snowFreezing);
  w.write_varuint(weather.snowFlakes);
  world_weather_detail::write_float(w, weather.fog);
  world_weather_detail::write_float(w, weather.snowFlakeSize);
  world_weather_detail::write_float(w, weather.snowStorm);
  return w.bytes();
}

inline bool decode_world_weather(const std::vector<std::uint8_t>& p, WorldWeather& weather) {
  if (p.empty() || p[0] != kWorldWeatherTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  WorldWeather decoded;
  if (!world_weather_detail::read_text(r, decoded.preset) ||
      !world_weather_detail::read_text(r, decoded.previous)) {
    return false;
  }
  decoded.seconds = world_weather_detail::read_float(r);
  decoded.remaining = world_weather_detail::read_float(r);
  decoded.rainStrength = world_weather_detail::read_float(r);
  decoded.rainLevel = world_weather_detail::read_float(r);
  decoded.windX = world_weather_detail::read_float(r);
  decoded.windY = world_weather_detail::read_float(r);
  decoded.windZ = world_weather_detail::read_float(r);
  decoded.snowAmount = world_weather_detail::read_float(r);
  decoded.snowFrost = world_weather_detail::read_float(r);
  decoded.snowFreezing = world_weather_detail::read_float(r);
  const std::uint64_t flakes = r.read_varuint();
  if (r.overrun() || flakes > kMaxSnowFlakes) {
    return false;
  }
  decoded.snowFlakes = std::uint32_t(flakes);
  // The fog and the flakes' size and storm, from servers that send them.
  if (r.bits_remaining() >= 3 * 32) {
    decoded.fog = world_weather_detail::read_float(r);
    decoded.snowFlakeSize = world_weather_detail::read_float(r);
    decoded.snowStorm = world_weather_detail::read_float(r);
  }
  if (r.overrun() || !world_weather_detail::valid(decoded)) {
    return false;
  }
  weather = std::move(decoded);
  return true;
}

}  // namespace kcd2s::integration
