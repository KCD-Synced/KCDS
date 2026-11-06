// The pillory's frame tick: the server's Player.putInPillory and
// releaseFromPillory for the local player, and the pillory reports sent back.
#pragma once

#include "../core/frame.hpp"

namespace kcds::game_client {

// Called by frame() every frame.
void tick_pillory(const Frame& frame);

}  // namespace kcds::game_client
