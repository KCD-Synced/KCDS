// Guards for the player replicas: left out of the AI LOD update, and no
// combat animation queued while one is removed; installed at startup.
#pragma once

namespace kcds::game_client {

// Called by initialize_adapter(): a remote player's handler state kept from locking the local
// input. Optional; logged when unavailable.
void install_player_state_guard();
// Called by initialize_adapter().
void install_replica_guards();

}  // namespace kcds::game_client
