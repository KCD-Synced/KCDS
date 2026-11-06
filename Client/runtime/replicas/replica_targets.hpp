// The replicas' movement requests, AI objects and transforms, found at startup before MinHook is
// initialized.
#pragma once

#include "../core/startup_targets.hpp"

namespace kcds::game_client {

// Called by initialize_adapter().
void resolve_replica_targets(StartupTargets& targets);
// Called by initialize_adapter(): the NPC lookup and the replicas' autonomy hook. Returns 0, or
// 1 when the adapter must not start (its exit code).
DWORD install_replica_autonomy();
// Called by initialize_adapter() after MinHook is initialized: the replicas' movement requests
// and AI objects. Returns 0, or 1 when a hook failed and the adapter must not start (its exit
// code).
DWORD install_replica_hooks(const StartupTargets& targets);

}  // namespace kcds::game_client
