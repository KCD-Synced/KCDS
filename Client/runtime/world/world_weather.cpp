#include "world_weather.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

#include "../winter/snow_stage.hpp"
#include "fog.hpp"

namespace kcds::runtime::world {
namespace {

using GetEnvironment = void* (*)();
using GetWeatherSystem = void* (*)(void* manager);
using ChangeWeather = void (*)(void* weather_system, const char* profile,
                               std::int64_t blend_milliseconds, bool instant);
using SetWind = void (*)(void* engine, const float* wind);

constexpr std::size_t kEnvironmentManager = 312;  // the environment module's object holding it
constexpr std::size_t kSlotGetWeatherSystem = 8;  // that object's vtable slot
constexpr std::size_t kSlotChangeWeather = 1;     // the weather system's vtable slot
constexpr std::size_t kSlotSetWind = 158;         // the 3D engine's vtable slot

// How often the server's preset is put in again, and the blend it is put in with (game
// milliseconds): the same profile again changes nothing, another one fades over a minute.
constexpr std::chrono::seconds kKeepInterval{120};
constexpr std::int64_t kKeepBlendMilliseconds = 60000;
// After the server's weather is applied (a join, a reload or a new order), its preset and wind
// are put in again every kSettleInterval for kSettleWindow, so the game's own weather cannot win
// over it while the level settles. The blend is short for the same reason.
constexpr std::chrono::seconds kSettleWindow{20};
constexpr std::chrono::seconds kSettleInterval{1};
constexpr std::int64_t kSettleBlendMilliseconds = 1000;

constexpr std::size_t kNameCapacity = kcd2s::integration::kMaxWeatherPresetLength + 1;
// How many preset names are kept: the game may read a name after the call returns.
constexpr std::size_t kNameSlots = 4;

// A preset name as the game's string type holds it: the characters behind a 12 byte header whose
// reference count of -1 marks them as never freed.
struct GameString {
  std::int32_t references = -1;
  std::int32_t length = 0;
  std::int32_t capacity = 0;
  char characters[kNameCapacity] = {};
};

WeatherNatives natives;
Logger report = nullptr;
std::array<GameString, kNameSlots> names;
std::size_t next_name = 0;
std::string last_preset;
float last_wind[3] = {0.0f, 0.0f, 0.0f};
bool wind_set = false;
// When the server's preset is put in again next; unset until the server's weather came.
std::chrono::steady_clock::time_point next_keep{};
bool keeping = false;
// The weather the server sent and that the game has not taken yet (its weather system may be
// missing); applied by apply_held() in a later frame.
std::optional<kcd2s::integration::WorldWeather> held;
// The last weather applied; the settle window puts its preset and wind in again.
std::optional<kcd2s::integration::WorldWeather> applied;
// The settle window after an apply: until when it lasts and when the next put-in is due.
bool settling = false;
std::chrono::steady_clock::time_point settle_until{};
std::chrono::steady_clock::time_point next_settle{};

void** vtable_of(void* object) {
  return *static_cast<void***>(object);
}

void* weather_system() {
  if (!natives.get_environment) {
    return nullptr;
  }
  auto* environment =
      static_cast<std::uint8_t*>(reinterpret_cast<GetEnvironment>(natives.get_environment)());
  if (!environment) {
    return nullptr;
  }
  void* manager = *reinterpret_cast<void**>(environment + kEnvironmentManager);
  if (!manager) {
    return nullptr;
  }
  return reinterpret_cast<GetWeatherSystem>(vtable_of(manager)[kSlotGetWeatherSystem])(manager);
}

const char* keep_name(const std::string& preset) {
  GameString& slot = names[next_name];
  next_name = (next_name + 1) % kNameSlots;
  slot.length = std::int32_t(preset.size());
  slot.capacity = std::int32_t(kNameCapacity);
  std::memcpy(slot.characters, preset.c_str(), preset.size() + 1);
  return slot.characters;
}

void change_preset(void* system, const kcd2s::integration::WorldWeather& weather) {
  // Game milliseconds; 0 changes the weather at once.
  const std::int64_t blend = std::int64_t(double(weather.remaining) * 1000.0);
  reinterpret_cast<ChangeWeather>(vtable_of(system)[kSlotChangeWeather])(
      system, keep_name(weather.preset), blend, blend == 0);
  last_preset = weather.preset;
  if (report) {
    report("WORLD WEATHER %s over %.1f s (blend of %.1f s)", weather.preset.c_str(),
           double(weather.remaining), double(weather.seconds));
  }
}

void change_wind(void* engine, const kcd2s::integration::WorldWeather& weather) {
  const float wind[3] = {weather.windX, weather.windY, weather.windZ};
  reinterpret_cast<SetWind>(vtable_of(engine)[kSlotSetWind])(engine, wind);
  std::memcpy(last_wind, wind, sizeof(wind));
  wind_set = true;
  if (report) {
    report("WORLD WIND %.2f %.2f %.2f", double(wind[0]), double(wind[1]), double(wind[2]));
  }
}

// Inside the settle window the server's preset and wind are put in again, once per
// kSettleInterval.
void settle(std::chrono::steady_clock::time_point now) {
  if (now < next_settle) {
    return;
  }
  void* system = weather_system();
  void* engine = natives.engine_3d_slot ? *natives.engine_3d_slot : nullptr;
  if (!system || !engine || !applied) {
    return;
  }
  next_settle = now + kSettleInterval;
  reinterpret_cast<ChangeWeather>(vtable_of(system)[kSlotChangeWeather])(
      system, keep_name(applied->preset), kSettleBlendMilliseconds, false);
  change_wind(engine, *applied);
}

}  // namespace

bool point_indoors(const float point[3]) {
  constexpr std::size_t kSlotIsPointIndoors = 1296 / 8;
  using IsPointIndoors = std::uint64_t (*)(void* engine, const float* point);
  void* engine = natives.engine_3d_slot ? *natives.engine_3d_slot : nullptr;
  if (!engine) {
    return false;
  }
  const auto call = reinterpret_cast<IsPointIndoors>(vtable_of(engine)[kSlotIsPointIndoors]);
  return (call(engine, point) & 0xFF) != 0;
}

float rain_intensity() {
  constexpr std::size_t kWeatherModule = 312;
  constexpr std::size_t kSlotWeather = 64 / 8;
  constexpr std::size_t kSlotRain = 72 / 8;
  if (!natives.get_environment) {
    return 0.0f;
  }
  auto* environment =
      static_cast<std::uint8_t*>(reinterpret_cast<GetEnvironment>(natives.get_environment)());
  void* module = environment ? *reinterpret_cast<void**>(environment + kWeatherModule) : nullptr;
  if (!module) {
    return 0.0f;
  }
  using GetObject = void* (*)(void*);
  using GetRain = float (*)(void*);
  void* weather = reinterpret_cast<GetObject>(vtable_of(module)[kSlotWeather])(module);
  if (!weather) {
    return 0.0f;
  }
  const float rain = reinterpret_cast<GetRain>(vtable_of(weather)[kSlotRain])(weather);
  return std::isfinite(rain) ? std::clamp(rain, 0.0f, 1.0f) : 0.0f;
}

namespace {

// The falling snow without a storm (the snow stage's own defaults) and in a full one.
constexpr float kCalmSnowWind = 0.1f;
constexpr float kStormSnowWind = 2.0f;
constexpr float kCalmSnowGravity = 0.1f;
constexpr float kStormSnowGravity = 0.5f;
constexpr float kCalmSnowTurbulence = 0.5f;
constexpr float kStormSnowTurbulence = 2.0f;
constexpr float kCalmSnowSwirl = 0.1f;
constexpr float kStormSnowSwirl = 0.5f;

// The server's snow (World.setSnow) to the renderer's snow stage, when it changed.
void change_snow(const kcd2s::integration::WorldWeather& weather) {
  static bool told = false;
  static kcd2s::integration::WorldWeather last;
  const bool same =
      told && last.snowAmount == weather.snowAmount && last.snowFrost == weather.snowFrost &&
      last.snowFreezing == weather.snowFreezing && last.snowFlakes == weather.snowFlakes &&
      last.snowFlakeSize == weather.snowFlakeSize && last.snowStorm == weather.snowStorm;
  if (same) {
    return;
  }
  winter::snow::Settings snow;
  snow.amount = weather.snowAmount;
  snow.frost = weather.snowFrost;
  snow.freezing = weather.snowFreezing;
  snow.flakes = int(weather.snowFlakes);
  snow.flake_size = weather.snowFlakeSize;
  // The storm drives the flakes: from the stage's calm defaults (storm 0) the wind's hold on them,
  // their fall and their swirl grow to a blizzard's at 1.
  const float storm = weather.snowStorm;
  snow.wind = kCalmSnowWind + storm * (kStormSnowWind - kCalmSnowWind);
  snow.gravity = kCalmSnowGravity + storm * (kStormSnowGravity - kCalmSnowGravity);
  snow.turbulence = kCalmSnowTurbulence + storm * (kStormSnowTurbulence - kCalmSnowTurbulence);
  snow.turbulence_frequency = kCalmSnowSwirl + storm * (kStormSnowSwirl - kCalmSnowSwirl);
  winter::snow::set(snow);
  told = true;
  last = weather;
  if (report) {
    report("WORLD SNOW amount %.2f frost %.2f freezing %.2f flakes %u size %.2f storm %.2f",
           weather.snowAmount, weather.snowFrost, weather.snowFreezing, weather.snowFlakes,
           weather.snowFlakeSize, weather.snowStorm);
  }
}

}  // namespace

bool install_weather(const WeatherNatives& found, Logger logger) {
  natives = found;
  report = logger;
  return natives.get_environment && natives.engine_3d_slot;
}

bool apply(const kcd2s::integration::WorldWeather& weather) {
  void* system = weather_system();
  void* engine = natives.engine_3d_slot ? *natives.engine_3d_slot : nullptr;
  if (!system || !engine) {
    return false;
  }
  if (weather.preset != last_preset) {
    change_preset(system, weather);
  }
  // Kept once the blend has had its time: a game second is at most a real one at the usual
  // clock speeds.
  const auto blend = std::chrono::seconds(std::int64_t(std::ceil(double(weather.remaining))));
  next_keep = std::chrono::steady_clock::now() + blend + kKeepInterval;
  keeping = true;
  const bool same_wind = wind_set && last_wind[0] == weather.windX &&
                         last_wind[1] == weather.windY && last_wind[2] == weather.windZ;
  if (!same_wind) {
    change_wind(engine, weather);
  }
  change_snow(weather);
  fog::want(weather.fog);
  // The settle window starts over with every weather the server sends.
  const auto now = std::chrono::steady_clock::now();
  applied = weather;
  settling = true;
  settle_until = now + kSettleWindow;
  next_settle = now + kSettleInterval;
  return true;
}

void hold(const kcd2s::integration::WorldWeather& weather) {
  held = weather;
}

void set_snow_and_fog(const kcd2s::integration::WorldWeather& weather) {
  change_snow(weather);
  fog::want(weather.fog);
}

void apply_held() {
  if (held && apply(*held)) {
    held.reset();
  }
  winter::snow::settle();
  fog::frame(natives.engine_3d_slot ? *natives.engine_3d_slot : nullptr);
}

void keep() {
  if (!keeping || last_preset.empty()) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (settling) {
    if (now < settle_until) {
      settle(now);
      return;
    }
    settling = false;
  }
  if (now < next_keep) {
    return;
  }
  void* system = weather_system();
  if (!system) {
    return;
  }
  next_keep = now + kKeepInterval;
  reinterpret_cast<ChangeWeather>(vtable_of(system)[kSlotChangeWeather])(
      system, keep_name(last_preset), kKeepBlendMilliseconds, false);
  if (report) {
    report("WORLD WEATHER %s kept", last_preset.c_str());
  }
}

void forget() {
  keeping = false;
  last_preset.clear();
  wind_set = false;
  // The world went: a weather held from it is not applied to the next one.
  held.reset();
  applied.reset();
  settling = false;
}

}  // namespace kcds::runtime::world
