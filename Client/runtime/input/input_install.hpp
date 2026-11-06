// The local player's input at startup: the hooks of its interactive state and its
// action maps.
#pragma once

#include <windows.h>

#include "../core/startup_targets.hpp"

namespace kcds::game_client {

// Called by initialize_adapter() after MinHook is initialized. Returns 0, or 1 when a hook
// failed and the adapter must not start (its exit code).
DWORD install_interactive_state_hooks(const StartupTargets& targets);
// Called by initialize_adapter() after MinHook is initialized: the local player's input
// actions, found and hooked for the chat (the native chat reads targets.player_input_action).
// Returns 0, or 1 when the adapter must not start (its exit code).
DWORD install_player_input_hook(StartupTargets& targets);

}  // namespace kcds::game_client
