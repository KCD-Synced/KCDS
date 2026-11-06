// World interactions a resource puts on things (the server's World.addInteraction;
// INTERACTIONS in kcd2s/integration/interactions.hpp): the game's own "press [key] to ..." hint
// (action_hints' engine hint) shows while the local player is near one and looks at it, and using
// it is reported to the server, which runs the resource's handler.
//
// A target is a level entity found by its name (IEntitySystem + 128 FindEntityByName, its world
// matrix at slot 32) or a position. The player aims at it when the camera's ray (where the camera
// looks, up and down too) meets the target within the interaction's radius: the box the entity
// draws (its render component's local bounds, turned and placed as the entity is; nothing solid
// may be before it), or for a target that draws none (a helper entity such as a pillory's holder,
// a position) an upright cylinder of the interaction's target radius and height standing on it:
// what the game's own ray test first hits must lie in it (the ground under it does not count), or
// with nothing solid ahead the ray must pass through it. Of several, the nearest shows. Engine
// thread only, except take_fired.
#pragma once

#include <cstdint>
#include <vector>

#include "kcd2s/integration/interactions.hpp"

namespace kcds::runtime::interactions {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* (*entity_system)() = nullptr;  // IEntitySystem
  void* component = nullptr;           // an entity's component by its id
  // A player's or NPC's body entity by its network id, null when this game has none.
  void* (*body_of)(std::uint32_t network_id) = nullptr;
};

void install(const Natives& natives, Logger logger);

// The server's new list for the local player.
void set_list(std::vector<kcd2s::integration::Interaction> list);

// Every frame: `view` is the view camera's matrix (null when there is none); `allowed` is false
// while a menu, the chat or another UI owns the input. Shows or takes away the hint.
void frame(const float* view, bool allowed);

// The interactions the player used since the last call, to report to the server.
std::vector<std::uint32_t> take_fired();

// The world is gone: no interactions, no hint.
void reset();

}  // namespace kcds::runtime::interactions
