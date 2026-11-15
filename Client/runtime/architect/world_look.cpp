#include "world_look.hpp"

#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "../world/world_clock.hpp"
#include "../winter/snow_stage.hpp"
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
  float snow_flake_size = 1.0f;
  float snow_storm = 0.0f;
  // Fog over the weather's own (0 none, up to 1).
  float fog = 0.0f;
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

// The editor's snow { amount, frost, freezing, flakes, flakeSize, storm } and fog; false for a part
// out of range.
bool snow_of(const sdk::Value& message, Look& look) {
  const auto within = [](double value, double least, double most) {
    return std::isfinite(value) && value >= least && value <= most;
  };
  const double fog = number(message, "fog", 0.0);
  if (!within(fog, 0.0, 1.0)) {
    return false;
  }
  look.fog = float(fog);
  const sdk::Value* snow = integration::script_field(message, "snow");
  if (!snow) {
    return true;
  }
  const double amount = number(*snow, "amount", 0.0);
  const double frost = number(*snow, "frost", 0.0);
  const double freezing = number(*snow, "freezing", 0.0);
  const double flakes = number(*snow, "flakes", 0.0);
  const double size = number(*snow, "flakeSize", 1.0);
  const double storm = number(*snow, "storm", 0.0);
  if (!within(amount, 0.0, kMostSnowAmount) || !within(frost, 0.0, 1.0) ||
      !within(freezing, 0.0, 1.0) || !within(flakes, 0.0, kMostSnowFlakes) ||
      !within(size, integration::kMinSnowFlakeSize, integration::kMaxSnowFlakeSize) ||
      !within(storm, 0.0, 1.0)) {
    return false;
  }
  look.snow_amount = float(amount);
  look.snow_frost = float(frost);
  look.snow_freezing = float(freezing);
  look.snow_flakes = std::uint32_t(flakes);
  look.snow_flake_size = float(size);
  look.snow_storm = float(storm);
  return true;
}

// The look's snow and fog in a weather.
void put_snow(const Look& look, kcd2s::integration::WorldWeather& sky) {
  sky.snowAmount = look.snow_amount;
  sky.snowFrost = look.snow_frost;
  sky.snowFreezing = look.snow_freezing;
  sky.snowFlakes = look.snow_flakes;
  sky.snowFlakeSize = look.snow_flake_size;
  sky.snowStorm = look.snow_storm;
  sky.fog = look.fog;
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "architectWorld") {
    return false;
  }
  if (!local_mode::enabled()) {
    return true;
  }
  // The editor's screen is over the world it shows: the flakes fall under it.
  winter::snow::flakes_under_screens(false);
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
    // The game's own weather again: the editor's snow goes, its fog stays.
    kcd2s::integration::WorldWeather sky;
    put_snow(*snow, sky);
    world::set_snow_and_fog(sky);
  }
  world::apply_held();
  // The preset again before the game's own weather timeline takes over.
  world::keep();
}

}  // namespace kcds::runtime::architect::world_look
