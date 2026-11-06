// The combat functions found at startup before MinHook is initialized: targets,
// input, hits, sync attacks, clinches, RPG events, the remote actors' lifecycle, the effects of
// hits, and the local player's interactive state and action maps.
#pragma once

#include <windows.h>

#include "../core/startup_targets.hpp"

namespace kcds::game_client {

// Called by initialize_adapter().
void resolve_combat_targets(StartupTargets& targets);
// Called by initialize_adapter() before MinHook is initialized: the neutral combat entry, the
// body's combat entry, the animation progress, the attack lifecycle, the denials, the movement
// restriction, the sync attacks, the HUD's combat input and the map concept. Returns 0, or 1
// when the adapter must not start (its exit code).
DWORD resolve_neutral_combat_targets(StartupTargets& targets);
// Called by initialize_adapter() after MinHook is initialized, in this order. Each returns 0,
// or 1 when a hook failed and the adapter must not start (its exit code).
DWORD install_neutral_attack_hooks(const StartupTargets& targets);
DWORD install_neutral_combat_hooks(const StartupTargets& targets);
// Called by initialize_adapter() later, in this order: the native event's modify and queue,
// the combat hooks (collision, melee, hit feedback and reaction, sync attacks, clinches,
// attacks, combat queries, perfect block window, RPG events, melee hit, combat target), and
// after the optional guards the combat input, eligibility, damage, characteristic and the
// player's constructor. Each returns 0, or 1 when a hook failed and the adapter must not start.
DWORD install_native_hit_hooks(const StartupTargets& targets);
DWORD install_combat_hooks(const StartupTargets& targets);
DWORD install_combat_input_hooks(const StartupTargets& targets);

}  // namespace kcds::game_client
