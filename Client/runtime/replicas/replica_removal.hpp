// Removing a replica's entity without harming the local player: the GUI module's unbind is
// skipped meanwhile, the replica's stale UI state listener is silenced, and the local player's UI
// binding is restored after the removal.
#pragma once

#include <cstdint>

#include "kcd2s/core/ids.hpp"

namespace kcds::game_client {

using ClientPlayerUnbind = void*(__fastcall*)(void*);
extern ClientPlayerUnbind original_client_player_unbind;
extern bool removing_remote_replica;
void* __fastcall hooked_client_player_unbind(void* manager);
// Removes a replica's entity; the number of combat animations left out meanwhile.
unsigned remove_replica_entity(void* entity_system, std::uint32_t engine_id, void* actor,
                               kcd2s::core::EntityId player);
// The full-screen game UI (inventory, map, journal) is open.
bool local_full_screen_ui();
void silence_remote_ui_state_listener(void* remote_actor, void* local_actor);

// The local player's UI binding, captured before a removal and restored after it.
struct LocalUiBinding {
  void* actor = nullptr;
  void* ui = nullptr;
  const void* ui_vtable = nullptr;
};
LocalUiBinding capture_local_ui(void* local_actor);
void restore_local_ui(const LocalUiBinding& binding, void* local_actor);

}  // namespace kcds::game_client
