// Sharpening a weapon on a grindstone, seen and heard by every player, WHGame 1.5.5.
//
// The owner's game runs the minigame: C_Sharpening (RTTI ".?AVC_Sharpening@playermodule@wh@@",
// vtable slot 19 its frame update, which takes the frame time) keeps the user's actor at
// + 24, whether it runs at + 44 and the grindstone (a Grindstone entity) by id at + 224. The wheel
// is a clip on the grindstone's own character: once the player pedals (the wheel's speed at
// + 172) the minigame starts "grindstone_spinning_<suffix>" on it and keeps its time
// at the sharpener's own clip time; with the wheel at rest it stops it.
// The sparks are two emitters the minigame makes at the character's joint "sword_pos"
// (through): "particles_smithery.sharpening" for a good contact (+ 272) and
// "...sharpening_fail" for a bad one (+ 280); every frame it gives each its spawn parameters
// (the good one's count scale, the bad one's). The grinding sound is the trigger
// "f_bl_sharpening_whetstone_main" on the grindstone's audio, shaped by the audio parameters
// bl_whetstone_speed, bl_sanding_pressure and bl_sanding_damage the minigame sets on it.
//
// The owner's client reports the wheel's clip, the grindstone's sounds and audio parameters and
// the sparks' spawn parameters in the NetworkPlayer field "stationAction", kind "grind"
// (kcd2s/integration/station_action.hpp). Every other client plays the clip on its own grindstone
// at the replica's own clip time (the game does not run a level entity's clip on another client,
// smithing.hpp), starts the same sounds with the same parameters on it, and makes the same two
// emitters with the owner's spawn parameters where the owner's game placed its own (relative to
// the grindstone: the minigame turns them away from the joint after its attachment placed them).
// When the field empties all of it goes.
//
// Engine thread only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace kcds::runtime::crafting::grinding {

using Logger = void (*)(const char* format, ...);

struct Natives {
  const std::uint8_t* image = nullptr;  // WHGame.dll as loaded
  std::size_t image_size = 0;
  // The particle manager: slot 6 FindEffect(name, source, load), slot 14
  // DeleteEmitter; an effect's slot 6 Spawn(location, flags) makes an emitter where it is told.
  void* const* particle_manager = nullptr;
  Logger log = nullptr;
};

// Hooks the minigame's frame update (found by its RTTI name); false when it is missing.
bool install(const Natives& natives);

// Every frame on the owner's side: the field "stationAction" for the local player, "" while it
// sharpens nowhere.
std::string own_field(void* local_actor, void* local_entity, double now);

// Every frame for a replica: its body (null when it has none) shows the field's grinding.
void keep(std::uint32_t holder, void* body, const std::string& field, double now);

// The replica is gone: its grinding stops on this client.
void forget(std::uint32_t holder);

// The world was reset: nothing is shown any more.
void reset();

// Whether the local player's minigame runs at a grindstone now.
bool own_running();

}  // namespace kcds::runtime::crafting::grinding
