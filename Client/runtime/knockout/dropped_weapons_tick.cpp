// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "dropped_weapons_tick.hpp"

#include "../core/log.hpp"
#include "../replicas/remote.hpp"
#include "dropped_weapons.hpp"
#include "knockout.hpp"

namespace kcds::game_client {

void tick_dropped_weapons(void* local_actor) {
  // The weapons a knocked-out body lets go of (knockout/dropped_weapons.hpp); key 0 is the local
  // player's own body, network ids are never 0. A fainted body is down the same way,
  // as its replicas are (replica_down).
  kcds::runtime::knockout::dropped_weapons::tick(
      0, local_actor, kcds::runtime::knockout::knocked_out() || kcds::runtime::knockout::fainted(),
      true, log);
  for (const auto& [id, remote] : remotes) {
    kcds::runtime::knockout::dropped_weapons::tick(
        id, remote.actor, kcds::runtime::knockout::replica_down(id), false, log);
  }
}

}  // namespace kcds::game_client
