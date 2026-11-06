// The native chat at startup: the show-console guard and the chat's install, on the
// local player's input action hook.
#pragma once

#include <windows.h>

#include "../core/startup_targets.hpp"

namespace kcds::game_client {

// Called by initialize_adapter() after the input action hook. Returns 0, or 1 when the adapter
// must not start (its exit code).
DWORD install_native_chat(const StartupTargets& targets);

}  // namespace kcds::game_client
