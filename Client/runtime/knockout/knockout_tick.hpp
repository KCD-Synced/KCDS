// The local player's knock-out each frame: reported to the server after the health
// that caused it.
#pragma once

namespace kcds::game_client {

void tick_knockout(void* local_actor);

}  // namespace kcds::game_client
