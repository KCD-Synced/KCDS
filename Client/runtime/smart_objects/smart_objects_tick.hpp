// The smart objects scripts place: their frame tick, the server's orders handed to
// smart_objects/placed_smart_objects.
#pragma once

#include "../core/frame.hpp"

namespace kcds::game_client {

// Called by frame() every frame.
void tick_smart_objects(const Frame& frame);

}  // namespace kcds::game_client
