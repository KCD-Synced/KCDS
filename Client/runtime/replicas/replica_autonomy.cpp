// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "replica_autonomy.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"

#include <mutex>
#include <unordered_map>

namespace kcds::game_client {

std::unordered_map<core::EntityId, Remote> remotes;

NativeNpcLookup native_npc_lookup = nullptr;
NativeNpcActor native_npc_actor = nullptr;
NativeAutonomyUpdate original_autonomy_update = nullptr;
std::mutex replica_autonomy_mutex;
std::unordered_map<void*, ReplicaAutonomyOwner> replica_autonomy_owners;

// IDA904CA8 updates the NPC context's autonomous state actions on native workers.
// Publish identities on the engine thread; workers only compare opaque addresses.
// A remote human's owner supplies intent. Its native combat/body action drivers
// remain active; this context must not submit a competing NPC recovery animation.
std::intptr_t __fastcall hooked_autonomy_update(void* context, const void* update) {
  core::EntityId player = 0;
  bool first = false;
  {
    std::lock_guard<std::mutex> lock(replica_autonomy_mutex);
    const auto owner = replica_autonomy_owners.find(context);
    if (owner != replica_autonomy_owners.end()) {
      player = owner->second.player;
      first = !owner->second.observed;
      owner->second.observed = true;
    }
  }
  if (!player) {
    return original_autonomy_update(context, update);
  }
  if (first) {
    log("REMOTE autonomy owner player=%u context=%p worker=%lu", player, context,
        GetCurrentThreadId());
  }
  return 0;
}

bool bind_replica_autonomy(core::EntityId player, Remote& remote) {
  if (GetCurrentThreadId() != engine_update_thread.load() || !local_view_wrapper ||
      !native_npc_lookup || !native_npc_actor ||
      slot<NativeNpcLookup>(local_view_wrapper, 4) != native_npc_lookup) {
    return false;
  }
  auto npc = static_cast<std::uint8_t*>(native_npc_lookup(local_view_wrapper, &remote.engine_id));
  if (!npc || native_npc_actor(npc) != remote.actor) {
    return false;
  }
  // C_NPC constructor BDC5F8 initializes context+40 to this NPC (BDCE0C).
  auto context = npc + 4336;
  if (*reinterpret_cast<void**>(context + 40) != npc) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(replica_autonomy_mutex);
    if (!replica_autonomy_owners.emplace(context, ReplicaAutonomyOwner{player}).second) {
      return false;
    }
  }
  remote.autonomy_context = context;
  log("REMOTE autonomy bound player=%u engine=0x%x actor=%p context=%p revision=%u", player,
      remote.engine_id, remote.actor, context, requested_revision);
  return true;
}

}  // namespace kcds::game_client
