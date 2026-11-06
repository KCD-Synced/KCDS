// The server's map revealed: the native fog of war lifted and its locations
// discovered once per loaded debug world, never during campaign startup.
#pragma once

#include <windows.h>

#include "kcd2s/integration/world_config.hpp"

namespace kcds::game_client {

// Called by initialize_adapter(): the fog reveal and discovery natives. Returns 0, or 1 when
// the adapter must not start (its exit code).
DWORD resolve_map_reveal_natives();
// Called by frame() every frame once the world is loaded.
void reveal_server_map(void* services, const kcd2s::integration::WorldConfig& world);

}  // namespace kcds::game_client
