// The single-player screens kept out of a server world: the skip-time screen and
// the tutorials, their hooks installed at startup.
#pragma once

#include <windows.h>

namespace kcds::game_client {

// Called by initialize_adapter(). Returns 0, or 1 when the adapter must not start (its exit
// code).
DWORD install_single_player_screen_hooks();

}  // namespace kcds::game_client
