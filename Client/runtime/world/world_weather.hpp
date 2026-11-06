// The world weather the server sets (kcd2s/integration/world_weather.hpp).
//
// The game's weather is a weather profile it blends to over game time. Its own script function
// Weather.ChangeWeather(BlendTime, Profile) makes the two calls this
// makes, no script runs:
//
//   - get_environment (the singleton getter) returns the environment module; its
//     object at + 312 answers vtable slot 8 (+ 64) with the weather system;
//   - the weather system's vtable slot 1 (+ 8) is ChangeWeather(system, profile name, blend in
//     game milliseconds, instant) and works out the profile by its name;
//   - the wind is the 3D engine's SetWind (vtable slot 158, + 1264), as the script binding
//     System.SetWind calls it, given a Vec3.
//
// The rain follows the preset's own profile: the game has no call to set the rain intensity or
// amount (only GetRainIntensity), so the server's rain values are not applied.
//
// The game's weather is a timeline of profiles it draws at random for itself. ChangeWeather
// (carried out) puts the profile in at the current time for six game hours
// (21600000 game milliseconds); after them the game's own timeline went on, different in every
// player's game. So the server's preset is put in again every two minutes, once a
// blend the server asked for has had its time; the same profile again changes nothing on screen.
//
// Engine thread.
#pragma once

#include "kcd2s/integration/world_weather.hpp"

namespace kcds::runtime::world {

using Logger = void (*)(const char* format, ...);

struct WeatherNatives {
  void* get_environment = nullptr;        // the environment module's getter
  void* const* engine_3d_slot = nullptr;  // the slot of the 3D engine global (I3DEngine*)
};

// False when a native is missing; the weather then stays the game's own.
bool install_weather(const WeatherNatives& natives, Logger logger);

// Changes the game's weather to the server's preset and sets its wind. A preset or wind that is
// already set is left alone; false while the game has no weather system or 3D engine yet.
bool apply(const kcd2s::integration::WorldWeather& weather);

// Holds a weather the server sent until apply_held() can apply it.
void hold(const kcd2s::integration::WorldWeather& weather);

// Engine thread, every frame in a world: applies the held weather, if any, once the game has its
// weather system.
void apply_held();

// Engine thread, every frame in a world: puts the server's preset in again when it is due, and
// during the settle window after an apply, puts the preset and wind in again every second.
void keep();

// Whether a point is inside a building (3D engine, vtable + 1296, as System.IsPointIndoors calls
// it). False before there is a 3D engine.
bool point_indoors(const float point[3]);

// How hard it rains, 0 to 1 (as the weather binding GetRainIntensity reads it).
float rain_intensity();

// The world went: nothing is kept or held until the server's weather comes again.
void forget();

}  // namespace kcds::runtime::world
