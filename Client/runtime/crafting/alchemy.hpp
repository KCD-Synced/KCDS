// Alchemy at an alchemy table, seen by every player, WHGame 1.5.5.
//
// The owner's game runs the minigame: C_Alchemy (RTTI ".?AVC_Alchemy@playermodule@wh@@", the
// same minigame base as smithing's: vtable slot 19 its frame update, the user's actor at + 24,
// the station, an AlchemyTable entity, at + 40 and whether it runs at + 44). The table keeps no
// state of its own (C_AlchemyTable serializes nothing): what the minigame does shows on the
// table's things, level entities standing at it: its AlchemyItem entities (mortar, pestle, bowl,
// pot, retort, bellows, sandglass, flasks, bottles, herbs), its ParticleEffect entities (the
// fires under the pot and the still, smoke, bubbles, splashes) and its Light entities (the
// fires' light). The minigame hides and shows them, moves them, gives them other meshes
// (CEntity slot 108 LoadGeometry, hooked) and switches their particles and lights; it spawns
// further entities for the herbs it grinds and boils.
//
// The owner's client notes every such thing as the session begins and reports how each one
// differs from that as entries of the NetworkPlayer field "stationAction" (kind "alchemy",
// kcd2s/integration/station_action.hpp); every other client finds the same things at its own
// table (by name and place), shows them so on the replica's table and puts them back when the
// field empties. What the body does travels as every animation does.
//
// Engine thread only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "kcd2s/integration/crafting_table.hpp"

namespace kcds::runtime::crafting::alchemy {

using Logger = void (*)(const char* format, ...);

struct Natives {
  const std::uint8_t* image = nullptr;  // WHGame.dll as loaded
  std::size_t image_size = 0;
  // A ParticleEffect entity's particle component turned on or off (whether it burns
  // at + 108) and the particle component of an entity, as smithing has them.
  void* particle_enable = nullptr;
  void* particle_component = nullptr;
  // A particle component activated: its effect loaded into a slot when it has none
  // (a fire the level keeps out has no emitter, and switching alone only marks it burning), then
  // switched on.
  void* particle_activate = nullptr;
  Logger log = nullptr;
};

// Hooks the minigame's frame update and the entities' LoadGeometry (found by their RTTI names);
// false when the minigame is missing.
bool install(const Natives& natives);

// Every frame on the owner's side: the field "stationAction" for the local player, "" while it
// is at no alchemy table.
std::string own_field(void* local_actor, void* local_entity, double now);

// Every frame for a replica: its body (null when it has none) shows the field's action when the
// field is of kind "alchemy"; any other field ends what it showed.
void keep(std::uint32_t holder, void* body, const std::string& field, double now);

// The replica is gone: what its action changed goes back.
void forget(std::uint32_t holder);

// The world was reset: the copies went with it and nothing is changed any more.
void reset();

// Whether a replica works at the table standing at that position (crafting permits keeps the
// local player from opening it meanwhile, so one player brews at a table).
bool table_taken(const float position[3]);

// Whether the local player's minigame runs at a station now.
bool own_running();

// What lies on the table the local player brews at (CRAFT_TABLE): the minigame keeps
// a list of its resources for each of the table's 21 places (C_Alchemy + 1136, 24 bytes each);
// a resource (C_AlchemyResource) holds its item (+ 8, the first item of its container: its class
// + 72, the class's GUID + 8), its flags (+ 16: 1 milled, 2 distilled) and whether it is a base
// liquid (+ 32). False while the local player brews at none.
bool table_contents(kcd2s::integration::CraftTable& table);

// The server's Crafting.cancel: the local player's minigame at a station exits as the player's
// own exit would; false when the local player is at none.
bool close_own();

}  // namespace kcds::runtime::crafting::alchemy
