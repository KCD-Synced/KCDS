// A world's per-world state dropped: the server sent another world, or the player leaves the world
// back to the main menu.
#pragma once

namespace kcds::game_client {

// Drops this world's replicas and per-world state (also declared in core/client_internal.hpp).
void reset_world_replicas(void* entity_system);

}  // namespace kcds::game_client
