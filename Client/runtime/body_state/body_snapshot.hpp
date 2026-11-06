// Test-only native observation of the players' bodies (KCDS_TEST_HOOKS): one bounded log
// snapshot of the local player's and the replicas' native body state per kit signal.
#pragma once

#include <cstdint>

#include "kcd2s/core/ids.hpp"

namespace kcds::game_client {

void tick_native_body_snapshot(void* entity_system, void* local_actor, unsigned local_id,
                               kcd2s::core::EntityId owner, std::uint32_t revision);

}  // namespace kcds::game_client
