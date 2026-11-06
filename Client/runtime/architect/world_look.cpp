#include "world_look.hpp"

#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "../world/world_clock.hpp"
#include "../world/world_weather.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "local_mode.hpp"

namespace kcds::runtime::architect::world_look {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

constexpr double kMostSpeed = 1000.0;
constexpr double kMostWind = 40.0;

struct Look {
  double hour = 12.0;
  double speed = 0.0;
  double wind = 0.0;
  std::string weather;
};

std::mutex look_mutex;
// What the editor asked for and not yet applied (time and weather apart: the calendar and the
// weather system come at different times).
std::optional<Look> pending_time;
std::optional<Look> pending_weather;

double number(const sdk::Value& value, const char* name, double fallback) {
  const sdk::Value* field = integration::script_field(value, name);
  if (!field) {
    return fallback;
  }
  if (field->tag() == sdk::Tag::Double) {
    return field->as_double();
  }
  if (field->tag() == sdk::Tag::Int) {
    return double(field->as_int());
  }
  return fallback;
}

std::string text(const sdk::Value& value, const char* name) {
  const sdk::Value* field = integration::script_field(value, name);
  return field && field->tag() == sdk::Tag::Str ? field->as_str() : std::string();
}

// A preset name as the game's weather table writes them: letters, digits and underscores.
bool preset_name(const std::string& name) {
  if (name.size() > integration::kMaxWeatherPresetLength) {
    return false;
  }
  for (const char c : name) {
    const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    const bool digit = c >= '0' && c <= '9';
    if (!letter && !digit && c != '_') {
      return false;
    }
  }
  return true;
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "architectWorld") {
    return false;
  }
  if (!local_mode::enabled()) {
    return true;
  }
  Look look;
  look.hour = number(message, "hour", 12.0);
  look.speed = number(message, "speed", 0.0);
  look.wind = number(message, "wind", 0.0);
  look.weather = text(message, "weather");
  if (!std::isfinite(look.hour) || look.hour < 0.0 || look.hour >= 24.0) {
    return true;
  }
  if (!std::isfinite(look.speed) || look.speed < 0.0 || look.speed > kMostSpeed) {
    return true;
  }
  if (!std::isfinite(look.wind) || look.wind < 0.0 || look.wind > kMostWind) {
    return true;
  }
  if (!preset_name(look.weather)) {
    return true;
  }
  std::lock_guard<std::mutex> lock(look_mutex);
  pending_time = look;
  if (!look.weather.empty()) {
    pending_weather = look;
  }
  return true;
}

void frame(bool in_world) {
  if (!in_world) {
    return;
  }
  std::optional<Look> time;
  std::optional<Look> weather;
  {
    std::lock_guard<std::mutex> lock(look_mutex);
    time = pending_time;
    weather = pending_weather;
    pending_weather.reset();
  }
  if (time) {
    const long long day = world::current_day();
    if (day >= 0) {
      kcd2s::integration::WorldTime clock;
      clock.day = static_cast<std::uint32_t>(day);
      clock.hour = static_cast<float>(time->hour);
      clock.timeScale = static_cast<float>(time->speed);
      if (world::apply(clock)) {
        std::lock_guard<std::mutex> lock(look_mutex);
        if (pending_time && pending_time->hour == time->hour &&
            pending_time->speed == time->speed) {
          pending_time.reset();
        }
      }
    }
  }
  if (weather) {
    kcd2s::integration::WorldWeather sky;
    sky.preset = weather->weather;
    sky.windX = static_cast<float>(weather->wind);
    // Held by the weather module until the game has its weather system.
    world::hold(sky);
  }
  world::apply_held();
  // The preset again before the game's own weather timeline takes over.
  world::keep();
}

}  // namespace kcds::runtime::architect::world_look
