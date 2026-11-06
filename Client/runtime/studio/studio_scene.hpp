// The Asset/Animation Studio's scene: World Architect's local mode (developer mode,
// no server, never on a server's world) with KCDS_ASSET_STUDIO=1 loads no game map, only the
// studio's own level kcds_studio (Dev/tools/assets/kcds_studio/make_studio_level.py): a flat
// 128 m floor and nothing else. The player is put in its middle once the world is ready.
//
// requested() any thread; stage_player() engine thread.
#pragma once

namespace kcds::runtime::studio {

// The studio's own level, which only the local mode names.
inline constexpr const char* kLevel = "kcds_studio";

// Whether the start asked for the studio (KCDS_ASSET_STUDIO=1 in the game's environment). The
// local mode decides whether it runs at all.
bool requested();

// Hands the game the studio's level from memory (assets/memory_paks): the .kcdsasset
// %LOCALAPPDATA%\KCDS\studio\kcds_studio.kcdsasset, sealed for this machine, opened in memory
// (assets/asset_container). The game's Data folder gets only an empty stub. False when the file
// is missing or does not open.
bool provide_level();

// Puts the local player in the middle of the studio's floor, once per world.
void stage_player(void* local_entity);

// Every frame in the studio's world: until the game has its calendar and weather, sets the
// studio's light once, noon with the clock stopped and a cloudless sky (the world clock and
// weather calls the server's time and weather use, Client/runtime/world).
void keep_light();

// The studio's Leave (engine thread): the next studio world stages the player and sets its light
// again, and the studio's console variables go back to the game's (studio_floor.hpp leave).
void leave();

}  // namespace kcds::runtime::studio
