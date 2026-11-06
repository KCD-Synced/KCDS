// The server's questions about this game's world (World.raycast, World.entitiesAround and the
// camera shots): the replicated bodies by engine id and where they stand.
#pragma once

#include <cstdint>
#include <vector>

#include "kcd2s/integration/world_ray.hpp"

namespace kcds::game_client {

std::uint32_t network_id_of_engine(std::uint32_t engine_id);
std::vector<kcd2s::integration::WorldBody> replicated_bodies();
void run_server_rays_and_shots(bool in_world);

}  // namespace kcds::game_client
