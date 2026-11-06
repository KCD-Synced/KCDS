// Rob on another player: the game's own CanBeRobbed and its calls found and installed at
// startup.
#pragma once

namespace kcds::game_client {

// Called by initialize_adapter().
void install_pickpocket();

}  // namespace kcds::game_client
