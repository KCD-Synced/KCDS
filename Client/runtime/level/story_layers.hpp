// The level's story layers in the game (WHGame 1.5.5): the states of parts of the map the story
// switches (a fortress before and after its siege, a forge ruined and rebuilt), each a layer of
// the level (layer_catalog.hpp). A layer is switched as the game's Engine:LayerSwitch flow node
// does it: gEnv->pEntitySystem's EnableLayer (vtable + 592: name, on, save), which turns its
// entities on or off and, through the 3D engine's ActivateObjectsLayer, its brushes, their
// collision and its static lights. IsLayerEnabled (vtable + 600: name, must be loaded) reads one.
// The level's own state of a layer is read before the first change, so it can be put back.
//
// Engine thread.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace kcds::runtime::level::layers {

using Logger = void (*)(const char* format, ...);

// entity_system: the global holding gEnv->pEntitySystem. False when it is missing.
bool install(void* const* entity_system, Logger logger);

// Whether the layer named `name` (its full name, with its GUID) is on now; nullopt when the game
// has no such layer.
std::optional<bool> enabled(const std::string& name);

// A level that switches its layers' objects itself (the KCD1 map, through the 3D engine by layer
// id): on or off, or std::nullopt for the state the level starts in. False when the level has no
// such layer. It goes first; the entity system then switches the layer's entities, if it has it.
using LayerFallback = bool (*)(const std::string& name, std::optional<bool> on);
void set_fallback(LayerFallback fallback);

// Turns the layer on or off; false when the game has no such layer (nor the fallback).
bool set(const std::string& name, bool on);

// Puts one layer changed here back as the level had it.
void restore(const std::string& name);

// Puts every layer changed here back as the level had it.
void restore_all();

// The world is left: the layers were its own, so nothing is put back, only forgotten.
void forget();

// A layer with something around a point.
struct NearLayer {
  std::uint16_t id = 0;
  std::string name;  // its full name
  std::string path;  // its place in the map's tree, or empty
  bool on = false;
  std::size_t brushes = 0;   // its brushes there, shown or hidden
  std::size_t shown = 0;     // of those, the ones drawn now
  std::size_t entities = 0;  // its entities in the whole map (from its file)
  float at[3] = {};          // the middle of what it has there
};

// The layers of `map` with brushes or entities within `radius` metres of `center`, nearest
// first, at most `most`. The level's files are read the first time a map is asked for.
std::vector<NearLayer> around(const std::string& map, const float center[3], float radius,
                              std::size_t most);

}  // namespace kcds::runtime::level::layers
