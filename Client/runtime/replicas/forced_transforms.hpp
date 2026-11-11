// Server-authored teleports (FORCE_XFORM) of the local player, applied on the engine thread.
#pragma once

#include <cstdint>

#include "kcd2s/core/ids.hpp"

namespace kcds::game_client {

// Applies the pending teleports; true when one moved the local player. A short correction of the
// local player by the movement check is not jumped but blended in (correction_blend.hpp).
bool apply_forced_transforms(void* entity_system, void* local_entity, std::uint32_t local_id,
                             kcd2s::core::EntityId own, std::uint32_t revision);

// Every frame, after apply_forced_transforms and before the local pose is captured: moves the
// local player on by this frame's part of a blended correction.
void apply_correction_blend(void* local_entity);

}  // namespace kcds::game_client
