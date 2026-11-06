// The shared ladders' frame tick: the other players on ladders and the local
// player's own climb handed to movement/shared_ladders.
#pragma once

#include "../core/frame.hpp"

namespace kcds::game_client {

// Called by frame() every frame.
void tick_ladders(const Frame& frame);

}  // namespace kcds::game_client
