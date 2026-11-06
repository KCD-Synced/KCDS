// A replica's spawn without harm to the local player: the movement request keeping a replica on its
// owner's heading, the spawn hook (also the level's items and cooking pots), and the local player's
// combat input and view selectors kept through a replica's creation and removal.
// retain_local_input, retain_local_view and local_view_wrapper are declared in
// core/client_internal.hpp.
#pragma once

#include "motion_capture.hpp"

namespace kcds::game_client {

// The entity system's spawn (hooked: the original), and set while a replica is being spawned.
using SpawnEntity = void*(__fastcall*)(void*, void*, bool);
extern SpawnEntity original_spawn;
extern thread_local bool spawning_remote;
// Set with spawning_remote for an owner whose body is a woman's.
extern thread_local bool spawning_female_remote;
void* __fastcall hooked_spawn(void* system, void* params, bool initialize);
// Called by frame() every frame until the entity system's spawn is hooked. Sets finished unless
// the hook failed (the adapter then stops, failed, and frame() returns at once).
void install_spawn_hook(void* entity_system, bool& finished);
// The local player's last movement request, and the hook that turns replicas to their owners.
extern NativeMovementRequest local_motion;
extern bool local_motion_valid;
void* __fastcall hooked_movement_request(void* animated, const NativeMovementRequest* request);
// The local view body and stealth watcher kept through a replica's lifetime, and their log.
extern void* local_view_body;
extern void* local_stealth_watcher;
extern void* local_stealth_player;
void diagnose_view(const char* event);

}  // namespace kcds::game_client
