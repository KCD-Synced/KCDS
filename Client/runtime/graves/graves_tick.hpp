// The level's graves' frame tick: the server's grave states handed to graves/graves,
// and the graves it reports sent to the server.
#pragma once

#include "../core/frame.hpp"

namespace kcds::game_client {

// Called by frame() every frame.
void tick_graves(const Frame& frame);

}  // namespace kcds::game_client
