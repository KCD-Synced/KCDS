// The bodies that are containers' frame tick: knocked-out players' and dead NPCs'
// bodies, and the server's stashes, where this game shows them.
#pragma once

#include "../core/frame.hpp"

namespace kcds::game_client {

// Called by frame() every frame.
void tick_body_containers(const Frame& frame);

}  // namespace kcds::game_client
