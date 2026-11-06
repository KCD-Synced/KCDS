// Whether the local player is inside a building, as the game itself knows it.
//
// The 3D engine's IsPointIndoors (world_weather.hpp point_indoors) answers only for vis areas, and
// the houses of Kuttenberg have none: every voice line inside a house said
// Outdoors. The game keeps its own flag: the game context "interior", set on the player while
// they are in a building. Its horse call refuses with ui_horse_indoor_disable on it (WHGame 1.5.5),
// which makes exactly these calls, no script runs:
//
//   - get_environment: the environment module; its object at + 0x160 holds at + 0xA8
//     the player's context source, whose vtable slot 2 (+ 0x10) returns an object whose slot 0
//     returns the player's key;
//   - contexts: the game context manager;
//   - make_string / free_string: the game's string around "interior";
//   - has_context: whether that key has that context.
//
// Engine thread.
#pragma once

namespace kcds::runtime::world::interior {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* get_environment = nullptr;
  void* contexts = nullptr;
  void* make_string = nullptr;
  void* has_context = nullptr;
  void* free_string = nullptr;
};

// False when a native is missing; player_inside() is then always false.
bool install(const Natives& natives, Logger logger);

// Whether the game has the local player in a building. False outside a world.
bool player_inside();

}  // namespace kcds::runtime::world::interior
