// The studio's floor. The level's flat terrain (Dev/tools/assets/kcds_studio/
// make_studio_level.py) carries the character but is not drawn in some starts, so the studio draws
// a floor mesh of its own over it: Dev/tools/assets/kcds_studio/make_studio_floor.py, embedded in
// the client (generated/studio_floor_files.hpp), mounted from memory at objects/kcds_studio/floor
// (native_ui mount_files) and spawned as a static mesh once in the studio's world. The toolbar's
// Floor switch hides and shows it.
//
// The engine's console variables are set through the console (IConsole::GetCVar, slot 23;
// ICVar::Set(int), slot 7; GetIVal, slot 2), as needs/needs.hpp sets wh_rpg_SkipFoodTimers:
//   - once in the studio's world: e_TerrainOcclusionCulling 0, so the floor's sectors are not
//     culled against themselves (the floor was missing in some starts though its physics stood);
//   - { op: 'studioFloor', on }: e_Terrain 1 or 0 (the terrain drawn or not).
//   - { op: 'studioCVar', name, value? } (the kit's tests, the assistant's studio_cvar): an int
//     console variable set, or read without a value; logged as "STUDIO cvar" and answered
//     { op: 'studioCVarValue', name, value } (null without the variable).
//   - once in the studio's world: ca_UseLookIK 0 and ca_LockFeetWithIK 0 (the animation's keys
//     are what the body does).
//
// handle_runtime_message: any thread; tick: engine thread.
#pragma once

#include <functional>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::studio::floor {

bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Every frame in the studio's world (send: to the resource runtime, the editor).
void tick(const std::function<bool(const kcd2s::sdk::Value&)>& send);

// The studio's Leave (engine thread): each console variable the studio changed gets the value it
// found back, so the next world (World Architect, a server) is drawn and animated as the game's
// own; the floor is made again in the next studio world.
void leave();

}  // namespace kcds::runtime::studio::floor
