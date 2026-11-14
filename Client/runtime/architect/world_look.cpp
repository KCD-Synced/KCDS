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

constexpr double kMostSnowAmount = 10.0;
constexpr double kMostSnowFlakes = 10000.0;

struct Look {
  double hour = 12.0;
  double speed = 0.0;
  double wind = 0.0;
  std::string weather;
  // The snow that comes with the weather (a blizzard), none by default.
  float snow_amount = 0.0f;
  float snow_frost = 0.0f;
  float snow_freezing = 0.0f;
  std::uint32_t snow_flakes = 0;
};

std::mutex look_mutex;
// What the editor asked for and not yet applied (time and weather apart: the calendar and the
// weather system come at different times). `pending_snow`: the snow alone, for the game's own
// weather.
std::optional<Look> pending_time;
std::optional<Look> pending_weather;
std::optional<Look> pending_snow;

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

// The editor's snow { amount, frost, freezing, flakes }; false for a part out of range.
bool snow_of(const sdk::Value& message, Look& look) {
  const sdk::Value* snow = integration::script_field(message, "snow");
  if (!snow) {
    return true;
  }
  const double amount = number(*snow, "amount", 0.0);
  const double frost = number(*snow, "frost", 0.0);
  const double freezing = number(*snow, "freezing", 0.0);
  const double flakes = number(*snow, "flakes", 0.0);
  const auto within = [](double value, double most) {
    return std::isfinite(value) && value >= 0.0 && value <= most;
  };
  if (!within(amount, kMostSnowAmount) || !within(frost, 1.0) || !within(freezing, 1.0) ||
      !within(flakes, kMostSnowFlakes)) {
    return false;
  }
  look.snow_amount = float(amount);
  look.snow_frost = float(frost);
  look.snow_freezing = float(freezing);
  look.snow_flakes = std::uint32_t(flakes);
  return true;
}

// The look's snow in a weather.
void put_snow(const Look& look, kcd2s::integration::WorldWeather& sky) {
  sky.snowAmount = look.snow_amount;
  sky.snowFrost = look.snow_frost;
  sky.snowFreezing = look.snow_freezing;
  sky.snowFlakes = look.snow_flakes;
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
  if (!preset_name(look.weather) || !snow_of(message, look)) {
    return true;
  }
  std::lock_guard<std::mutex> lock(look_mutex);
  pending_time = look;
  if (!look.weather.empty()) {
    pending_weather = look;
  } else {
    pending_snow = look;
  }
  return true;
}

void frame(bool in_world) {
  if (!in_world) {
    return;
  }
  std::optional<Look> time;
  std::optional<Look> weather;
  std::optional<Look> snow;
  {
    std::lock_guard<std::mutex> lock(look_mutex);
    time = pending_time;
    weather = pending_weather;
    snow = pending_snow;
    pending_weather.reset();
    pending_snow.reset();
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
    put_snow(*weather, sky);
    // Held by the weather module until the game has its weather system.
    world::hold(sky);
  } else if (snow) {
    // The game's own weather again: only the editor's snow goes (or stays, if it gave some).
    kcd2s::integration::WorldWeather sky;
    put_snow(*snow, sky);
    world::set_snow(sky);
  }
  world::apply_held();
  // The preset again before the game's own weather timeline takes over.
  world::keep();
}

}  // namespace kcds::runtime::architect::world_look
