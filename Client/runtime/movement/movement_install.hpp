// Movement: the movement restriction and the shared ladders' natives, found and installed at
// startup.
#pragma once

#include <windows.h>

namespace kcds::game_client {

// Called by initialize_adapter(). Returns 0, or 1 when the adapter must not start (its exit
// code).
DWORD install_movement();

}  // namespace kcds::game_client
