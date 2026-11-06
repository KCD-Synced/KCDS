// The skeleton's animation functions and the human overlay's natives, found at startup before
// MinHook is initialized.
#pragma once

#include <windows.h>

#include "../core/startup_targets.hpp"

namespace kcds::game_client {

// Called by initialize_adapter(). Returns 0, or 1 when the adapter must not start (its exit
// code).
DWORD resolve_animation_targets(StartupTargets& targets);
// Called by initialize_adapter() after MinHook is initialized: the skeleton's layer clear and
// stop, then (after the riders' body position hooks) the facial pak, the animation start and
// the motion parameters. Each returns 0, or 1 when a hook failed and the adapter must not start
// (its exit code).
DWORD install_animation_layer_hooks(const StartupTargets& targets);
DWORD install_animation_hooks(const StartupTargets& targets);

}  // namespace kcds::game_client
