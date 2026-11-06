// The local player's stats each frame, after the server's player commands.
#pragma once

#include "player_stats.hpp"

namespace kcds::game_client {

const kcds::runtime::stats::Fields& tick_stats(void* local_actor);

}  // namespace kcds::game_client
