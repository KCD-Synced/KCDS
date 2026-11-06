// The world clock and weather: the game's calendar and environment module, their
// natives found and installed at startup.
#pragma once

namespace kcds::game_client {

// Called by initialize_adapter().
void install_world_clock();

}  // namespace kcds::game_client
