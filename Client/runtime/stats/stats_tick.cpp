#include "stats_tick.hpp"

#include "../core/client_internal.hpp"

namespace kcds::game_client {

// The local player's stats for the fields "vitals" and "stats", after applying the
// server's setStat requests to its soul.
const kcds::runtime::stats::Fields& tick_stats(void* local_actor) {
  void* soul = multiplayer_world.load() && local_actor
                   ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(local_actor) + 1640)
                   : nullptr;
  run_player_commands();
  return kcds::runtime::stats::capture(soul, GetTickCount64());
}

}  // namespace kcds::game_client
