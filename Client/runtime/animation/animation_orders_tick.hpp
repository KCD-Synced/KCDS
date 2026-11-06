// The server's animation orders' frame tick: Player.playAnimation and
// stopAnimation played for the local player, and the ends of its requests sent back.
#pragma once

#include "../core/frame.hpp"

namespace kcds::game_client {

// Called by frame() every frame.
void tick_animation_orders(const Frame& frame);

}  // namespace kcds::game_client
