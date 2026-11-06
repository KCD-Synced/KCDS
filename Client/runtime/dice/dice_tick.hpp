// Dice matches between two players each frame: the server's states of the local player's
// match played at the game's table, and the local player's moves sent back.
#pragma once

namespace kcds::game_client {

void tick_dice();

}  // namespace kcds::game_client
