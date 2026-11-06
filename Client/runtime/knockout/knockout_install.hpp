// Knock-outs with the pillory, door and world entity hooks and the needs they come
// with, installed at startup.
#pragma once

#include <windows.h>

namespace kcds::game_client {

// Called by initialize_adapter(). Returns 0, or 1 when the adapter must not start (its exit
// code).
DWORD install_knockout_and_world_hooks();

}  // namespace kcds::game_client
