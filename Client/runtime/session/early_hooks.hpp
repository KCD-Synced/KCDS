// The hooks installed right after WHGame.dll is mapped: the user profile's load
// and the main menu's, and the server browser started when there is no launcher address.
#pragma once

#include "../core/startup_targets.hpp"

namespace kcds::game_client {

// Called by initialize_adapter() once WHGame.dll is mapped; sets targets.menu_hooks_installed.
void install_early_hooks(StartupTargets& targets);

}  // namespace kcds::game_client
