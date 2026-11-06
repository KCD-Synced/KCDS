// Riders and horses at startup: the body position hooks of seated riders and the
// player horses the server gives (riding/player_horse.hpp).
#pragma once

#include <windows.h>

#include "../core/startup_targets.hpp"

namespace kcds::game_client {

// Called by initialize_adapter() before MinHook is initialized: the horse natives, and the
// local player's mount and dismount for their hooks. Returns 0, or 1 when the adapter must not
// start (its exit code).
DWORD resolve_horse_targets(StartupTargets& targets);
// Called by initialize_adapter() before MinHook is initialized: the rider action's completion
// natives. Returns 0, or 1 when the adapter must not start (its exit code).
DWORD resolve_rider_completion_natives();
// Called by initialize_adapter() after MinHook is initialized: the local player's mount and
// dismount hooks, then the guard that keeps riders seated when an unrelated entity near them is
// removed. Each returns 0, or 1 when the adapter must not start (its exit code).
DWORD install_horse_hooks(const StartupTargets& targets);
DWORD install_seated_riders();
// Called by initialize_adapter().
void install_body_position_hooks();
// Called by initialize_adapter(): the guard of far riders' horse releases. Returns 0, or 1 when
// the adapter must not start (its exit code).
DWORD install_horse_reservations();
void install_player_horses();

}  // namespace kcds::game_client
