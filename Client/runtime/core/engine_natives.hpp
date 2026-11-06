// The engine's core natives, found at startup before anything else uses them:
// CCryAction::Update (hooked as the engine loop), the framework, the map callback and its
// console, the services getter, the main-menu return, the spawn parameters and CryString
// assignment. initialize_adapter() checks them before it installs the hooks.
#pragma once

namespace kcds::game_client {

// Called by initialize_adapter().
void resolve_engine_natives();

}  // namespace kcds::game_client
