// The Kingdom Come: Deliverance map (Rataje) as the level kcd1_rataje, read from the player's own
// copy of that game (install.hpp); nothing of it is shipped. KCD1's world keeps its own models,
// textures and materials and runs on KCD2's engine and mechanics.
//
//   - KCD1's Objects and Buildings packs and its assets pack (level_pack.hpp) are opened
//     at the game's root and moved to the front of CryPak's pack list, its lowest priority:
//     CryPak opens a pack at the end of that list (before the override packs, flag 0x400) and a
//     file is found in the last pack that has it. KCD2 reorganised its object folders, so the two
//     games share only a few paths (the engine's defaults), and those stay KCD2's.
//   - the level is listed as the custom maps are (custom_maps.hpp): a small marker pack holds
//     Levels/kcd1_rataje/level.pak for the level system's scan, and the level's pack opening
//     (CryPak's OpenPack, vtable + 56) opens the built level pack instead, bound at the level's
//     folder. That hook keeps the slot's previous function, so it works beside custom_maps'.
//
// mount: the engine's thread at startup. stage_player: the engine thread. The rest: any thread.
#pragma once

#include <string>

namespace kcds::runtime::kcd1 {

using Logger = void (*)(const char* format, ...);

// Builds the KCD1 packs when needed and mounts them, from the mod pak mounting
// (branding/runtime_branding.cpp) with CryPak and the game's binding root.
void mount(void* pak, const char* binding_root, Logger logger);

// Whether a level name is the KCD1 map's.
bool is_level(const std::string& level);

// Whether the game lists the KCD1 map (this PC has KCD1 and its packs are mounted).
bool listed();

// Puts the local player at Rataje when a world on the KCD1 map has loaded (nothing for another
// level): the level has none of KCD2's spawn points. As the script's SetWorldPos: the entity's
// world matrix (slot 32) with a new translation, set through SetWorldTM (slot 30).
void stage_player(const std::string& level, void* local_entity);

}  // namespace kcds::runtime::kcd1
