// Native ownership left out of the remote C_Actor release role.
#pragma once

namespace kcds::game_client {

// Install before any remote C_Player construction. Failure prevents adapter startup.
bool install_remote_interactor_lifetime();

// Use only for the remote C_Player primary vtable's Release slot (14), after installation.
// The wrapper preserves the native ActorRelease return value and exception behavior.
void* remote_interactor_release_target();

}  // namespace kcds::game_client
