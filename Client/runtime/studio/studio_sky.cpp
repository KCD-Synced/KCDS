#include "studio_sky.hpp"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "../core/log.hpp"
#include "../world/world_clock.hpp"
#include "../world/world_weather.hpp"
#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::studio::sky {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

// The studio's day (studio_scene.cpp keep_light).
constexpr std::uint32_t kStudioDay = 1;
constexpr std::size_t kLongestPreset = 64;

struct Sky {
  float hour = 12.0f;
  std::string preset = "cloudless_sunny";
  float rain = 0.0f;
};

std::mutex sky_mutex;
std::optional<Sky> wanted;

double number(const sdk::Value& message, const char* name, double fallback) {
  const sdk::Value* value = integration::script_field(message, name);
  if (value && value->tag() == sdk::Tag::Int) {
    return double(value->as_int());
  }
  if (value && value->tag() == sdk::Tag::Double) {
    return value->as_double();
  }
  return fallback;
}

bool preset_name(const std::string& name) {
  return !name.empty() && name.size() <= kLongestPreset &&
         std::all_of(name.begin(), name.end(), [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
         });
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "studioSky") {
    return false;
  }
  Sky next;
  next.hour = float(std::clamp(number(message, "hour", next.hour), 0.0, 23.99));
  next.rain = float(std::clamp(number(message, "rain", next.rain), 0.0, 1.0));
  const sdk::Value* preset = integration::script_field(message, "preset");
  if (preset && preset->tag() == sdk::Tag::Str && preset_name(preset->as_str())) {
    next.preset = preset->as_str();
  }
  std::lock_guard<std::mutex> lock(sky_mutex);
  wanted = next;
  return true;
}

void tick() {
  std::optional<Sky> next;
  {
    std::lock_guard<std::mutex> lock(sky_mutex);
    next.swap(wanted);
  }
  if (!next) {
    return;
  }
  kcd2s::integration::WorldTime time;
  time.day = kStudioDay;
  time.hour = next->hour;
  time.timeScale = 0.0f;
  const bool clock = world::apply(time);
  kcd2s::integration::WorldWeather weather;
  weather.preset = next->preset;
  weather.rainStrength = next->rain;
  weather.rainLevel = next->rain;
  world::hold(weather);
  game_client::log("STUDIO sky: %.1f h (clock %d), %s, rain %.2f", next->hour, int(clock),
                   next->preset.c_str(), next->rain);
}

}  // namespace kcds::runtime::studio::sky
