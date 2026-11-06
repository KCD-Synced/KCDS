// The level's doors' frame tick: the server's door states and questions handed to
// doors/doors.
#pragma once

#include "../core/frame.hpp"

namespace kcds::game_client {

// Called by frame() every frame.
void tick_doors(const Frame& frame);

}  // namespace kcds::game_client
