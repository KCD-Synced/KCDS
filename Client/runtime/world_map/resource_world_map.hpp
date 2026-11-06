// WorldMap for client resources: the game side of
// Client/resources/world_map.cjs.
//
// The player's own map marker is what the game calls the checkpoint (action map
// apse_map_checkpoint). WHGame 1.5.5: the map object M is the map module
// (cached by the game) + 5384; its checkpoints are
// shared_ptr<C_CheckpointMark> at M + 1416 (16 bytes each), one per map, the mark's map id at
// +120 and its position x, y, z at +80, +84, +88.(M, &mark) picks the current map's
// mark. The player sets or moves it(M) and removes it; both are
// hooked for the events, and clearWaypoint calls the removal itself. M + 1864 is set while the
// map screen is up.
//
// Compass marks: every mark the HUD's compass shows is added(mark, hud) and
// taken off; both are hooked and the marks on the strip are counted.
//
// Fast travel: the player module's C_FastTravel is *(services + 296) + 40; byte +72 is set
// while a travel runs (its Stop, tests it) and +116 holds the position along the
// route (the getter). Both offsets are checked against the game's code at startup.
// canStart is the game's own answer (travel_check.hpp), null until a player exists.
//
//   game -> runtime:  { op: "worldMap", open, waypoint: { x, y, z, mapId } | null,
//                       compassMarks, travel: { active, x, y, z, canStart } } on change
//                     { op: "worldMapEvent", name, where: { x, y, z, mapId }, moved }
//                     { op: "worldMapDiscovered", kind: "location" | "poi", id }
//   runtime -> game:  { op: "worldMapMove", x, y, z }, { op: "worldMapClear" }
//                     { op: "worldMapCategories", shown: [type], hidden: [type] }
#pragma once

#include <array>
#include <functional>
#include <vector>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::world_map {

using RuntimeSender = std::function<bool(const kcd2s::sdk::Value&)>;
using Logger = void (*)(const char* format, ...);

struct Natives {
  void* (*services)() = nullptr;  // the game services getter
  void* module_getter = nullptr;
  void* set_checkpoint = nullptr;
  void* remove_checkpoint = nullptr;
  void* refresh_hints = nullptr;
  void* redraw = nullptr;
  void* compass_add = nullptr;
  void* compass_remove = nullptr;
  bool travel_verified = false;  // the fast travel offsets match this build
  // Around a compass mark's add (the blips' own icons): called with the mark before it
  // is added, and after.
  void (*before_compass_add)(void* mark) = nullptr;
  void (*after_compass_add)() = nullptr;
};

bool install(const Natives& natives, Logger logger);

// The resources' choice for a mark type: 1 shown, 0 hidden, -1 when no resource chose.
int category_choice(int type);

// For local::Client::set_local_runtime_handler: true when the message was WorldMap's.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Engine thread, every frame: samples the map and the marker, runs queued moves and clears.
void frame(bool in_world);

// Whether a script asked to clear the whole level's fog of war (WorldMap.clearAllFog, issue)
// since the last call; the map reveal (map_reveal.cpp) lifts it on the engine thread.
bool take_fog_reveal();

// The points (x, y) scripts asked to clear the fog of war around since the last call
// (WorldMap.clearFog), oldest first; the map reveal clears them on the engine thread.
std::vector<std::array<float, 2>> take_fog_points();

// A place the game discovered: a location (a town, a district) or a point of
// interest, by its GUID as the game writes it (8-4-4-4-12 hex digits), from the discovery toast's
// bridge (markers/marker_hooks.cpp). Any thread.
void report_discovery(bool location, const char* guid);

// Network thread: reports state changes and events to the runtime of session.
void pump(const void* session, const RuntimeSender& send);

}  // namespace kcds::runtime::world_map
