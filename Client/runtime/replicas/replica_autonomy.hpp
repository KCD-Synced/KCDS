// A replica's NPC context: its autonomous state actions are left to its owner (the context is
// bound to the replica's owner and the native update skips it). remotes is defined here.
#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "remote.hpp"

namespace kcds::game_client {

// The NPC lookup of the view wrapper, an NPC's actor, and the autonomy update (hooked).
using NativeNpcLookup = void*(__fastcall*)(void*, const std::uint32_t*);
using NativeNpcActor = void*(__fastcall*)(void*);
using NativeAutonomyUpdate = std::intptr_t(__fastcall*)(void*, const void*);
extern NativeNpcLookup native_npc_lookup;
extern NativeNpcActor native_npc_actor;
extern NativeAutonomyUpdate original_autonomy_update;
// The replica owning each bound NPC context, and whether a native worker has seen it.
struct ReplicaAutonomyOwner {
  core::EntityId player = 0;
  bool observed = false;
};
extern std::mutex replica_autonomy_mutex;
extern std::unordered_map<void*, ReplicaAutonomyOwner> replica_autonomy_owners;
std::intptr_t __fastcall hooked_autonomy_update(void* context, const void* update);
bool bind_replica_autonomy(core::EntityId player, Remote& remote);

}  // namespace kcds::game_client
