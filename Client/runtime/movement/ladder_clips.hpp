// Replicas on a ladder: the climb clip held by hand where its owner's game holds it.
// ladder_climbers and local_held_climb are declared in core/client_internal.hpp.
#pragma once

#include <unordered_map>

#include "kcd2s/core/ids.hpp"

namespace kcds::game_client {

// The owner's held climb clip time, and the clip each replica's base layer holds, by network id.
extern std::unordered_map<kcd2s::core::EntityId, float> ladder_clip_times;
extern std::unordered_map<kcd2s::core::EntityId, int> ladder_held_clips;
void hold_ladder_clip(kcd2s::core::EntityId player, void* skeleton, int owner_clip);

}  // namespace kcds::game_client
