// The status effects on the local player's body each frame.
#pragma once

#include <string>

namespace kcds::game_client {

// The server's buff orders, and the body's effects to the server and the resource runtime.
void tick_buffs(void* local_actor);
// The local player's replicaBuffs report, read every 250 ms.
const std::string& tick_replica_buffs(void* local_actor);

}  // namespace kcds::game_client
