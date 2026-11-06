// World markers and blips: the game's decals and its points of interest on the
// compass and the map, their natives found and installed at startup.
#pragma once

#include <windows.h>

#include "../core/startup_targets.hpp"

namespace kcds::game_client {

// Called by initialize_adapter() before MinHook is initialized: the map's clouds, codex
// toasts, quest markers and discovery HUD. Returns 0, or 1 when the adapter must not start (its
// exit code).
DWORD resolve_map_ui_targets(StartupTargets& targets);
// Called by initialize_adapter() after MinHook is initialized: their hooks. Returns 0, or 1 when
// a hook failed and the adapter must not start (its exit code).
DWORD install_map_ui_hooks(const StartupTargets& targets);
// Called by initialize_adapter().
void install_markers();

}  // namespace kcds::game_client
