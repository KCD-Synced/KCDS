// The look's natives and hooks at startup: the look fades, the skeleton pose's look
// and the pose blender's look state and target.
#pragma once

#include <windows.h>

namespace kcds::game_client {

// Called by initialize_adapter() before MinHook is initialized. Returns 0, or 1 when the
// adapter must not start (its exit code).
DWORD resolve_look_natives();
// Called by initialize_adapter() after MinHook is initialized. Returns 0, or 1 when a hook
// failed and the adapter must not start (its exit code).
DWORD install_look_hooks();

}  // namespace kcds::game_client
