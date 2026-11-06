// Quitting the game and leaving a server's world for the main menu, both served on the engine
// thread when the browser asked for them.
#pragma once

namespace kcds::game_client {

void quit_if_requested();
void leave_world_if_requested();

}  // namespace kcds::game_client
