// The combat relay's frame tick: the local fighter's target, model and native
// actions relayed, and the combat events other clients relayed applied to this game.
#pragma once

#include "../core/frame.hpp"

namespace kcds::game_client {

// Called by frame() every frame, after the replicas were updated.
void tick_combat_relay(const Frame& frame);

}  // namespace kcds::game_client
