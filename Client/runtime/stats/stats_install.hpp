// Stats and the server's NPCs at startup: the soul's stat natives, the players'
// stats, the NPCs' health, fights, combat actors, walks and attacks.
#pragma once

#include <windows.h>

namespace kcds::game_client {

// Called by initialize_adapter(). Returns 0, or 1 when the adapter must not start (its exit
// code).
DWORD install_stats_and_npcs();

}  // namespace kcds::game_client
