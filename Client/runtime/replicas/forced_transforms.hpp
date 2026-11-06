// Server-authored teleports (FORCE_XFORM) of the local player, applied on the engine thread.
#pragma once

#include <cstdint>

#include "kcd2s/core/ids.hpp"

namespace kcds::game_client {

// Applies the pending teleports; true when one moved the local player.
bool apply_forced_transforms(void* entity_system, void* local_entity, std::uint32_t local_id,
                             kcd2s::core::EntityId own, std::uint32_t revision);

}  // namespace kcds::game_client
