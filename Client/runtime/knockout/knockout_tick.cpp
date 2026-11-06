#include "knockout_tick.hpp"

#include "../animation/player_animation.hpp"
#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../npcs/npc_attack.hpp"
#include "../stats/player_stats.hpp"
#include "knockout.hpp"

namespace kcds::game_client {

// A player never dies in a server world: a lethal hit knocks it out until a resource revives it
// (knockout/knockout.hpp).
void tick_knockout(void* local_actor) {
  void* soul = multiplayer_world.load() && local_actor
                   ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(local_actor) + 1640)
                   : nullptr;
  if (soul != pending_combat_soul) {
    discard_held_hits("local body changed");
    discard_player_damage("local body changed");
    kcds::runtime::player_animation::reset();
    pending_combat_soul = soul;
  }
  unsigned vision = 0;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    vision = client->peer().knockout_vision();
  }
  if (kcds::runtime::knockout::fall_took_last_health(soul, GetTickCount64())) {
    log("KNOCKOUT a fall took the last health");
    kcds::runtime::stats::deal_lethal(soul);
  }
  const bool knocked_out = kcds::runtime::knockout::tick(soul, vision);
  if (knocked_out) {
    // The NPCs this client simulates leave the knocked-out player be (npc_attack.hpp forgive).
    if (soul) {
      kcds::runtime::npcs::attack::forgive(
          *reinterpret_cast<const std::uint64_t*>(static_cast<std::uint8_t*>(soul) + 48));
    }
    report_committed_missiles_before_knockout();
    discard_held_hits("local knockout");
    // The health that knocked the player out reaches the server first (playerDamage before
    // playerDied, with the killer and the reason it gives).
    queue_killing_tick();
    send_player_damage();
  }
  if (knocked_out) {
    std::lock_guard<std::mutex> lock(network_mutex);
    client->peer().send_knocked_out();
  }
}

}  // namespace kcds::game_client
