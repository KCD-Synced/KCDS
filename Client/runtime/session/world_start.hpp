// The server's world started in this game: requested through the native
// NewGameDebug once the profile and its EULA are loaded, finished when the level is loaded, and
// the gameplay reported started when the loading screen goes down.
#pragma once

#include <cstdint>

#include "kcd2s/integration/world_config.hpp"

namespace kcds::game_client {

// Requests the server's world when its revision is new; frame() returns after it either way.
void request_server_world(void* framework, void* entity_system,
                          const kcd2s::integration::WorldConfig& world, double now);
// Finishes the world's start once its level is loaded. Sets finished when it did; otherwise the
// level is not loaded yet and frame() returns. last_diag is when frame() last logged readiness.
void finish_world_load(void* framework, void* services,
                       const kcd2s::integration::WorldConfig& world, double now, double last_diag,
                       std::uint32_t local_id, bool& finished);
// playerReady: reported once per world.
void report_gameplay_ready(const kcd2s::integration::WorldConfig& world, double now);

}  // namespace kcds::game_client
