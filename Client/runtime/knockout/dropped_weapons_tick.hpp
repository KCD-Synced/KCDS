// The dropped weapons' frame tick: the weapons the local player's and the replicas'
// knocked-out or fainted bodies let go of.
#pragma once

namespace kcds::game_client {

// Called by frame() every frame.
void tick_dropped_weapons(void* local_actor);

}  // namespace kcds::game_client
