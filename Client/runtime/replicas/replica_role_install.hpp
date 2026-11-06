// Replicas made without taking the player role or the missile input from the local player
// the hooks installed at startup.
#pragma once

namespace kcds::game_client {

// Called by initialize_adapter().
void install_replica_role();

}  // namespace kcds::game_client
