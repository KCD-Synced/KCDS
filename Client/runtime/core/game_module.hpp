// WHGame.dll at startup: waited for, the KCDS branding installed, and its image's
// bounds kept for the signature scans.
#pragma once

#include <windows.h>

namespace kcds::game_client {

// Called by initialize_adapter() first. Returns 0, or 1 when WHGame.dll did not load and the
// adapter must not start (its exit code).
DWORD find_game_module();

}  // namespace kcds::game_client
