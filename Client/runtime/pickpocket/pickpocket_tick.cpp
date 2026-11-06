#include "pickpocket_tick.hpp"

#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../inventory/stock_projection.hpp"
#include "../knockout/knockout.hpp"
#include "../physics/line_of_sight.hpp"
#include "../replicas/remote.hpp"

#include <cmath>
#include <vector>

namespace kcds::game_client {

// Where an entity stands and the way it faces, on the ground; false without its matrix.
bool body_of(void* entity, kcds::runtime::pickpocket::Body& body) {
  const float* m = entity ? slot<const float*(__fastcall*)(void*)>(entity, 32)(entity) : nullptr;
  if (!m) {
    return false;
  }
  body.position[0] = m[3];
  body.position[1] = m[7];
  body.position[2] = m[11];
  const float length = std::sqrt(m[1] * m[1] + m[5] * m[5]);
  if (!(length > 1e-3f)) {
    return false;
  }
  body.forward[0] = m[1] / length;
  body.forward[1] = m[5] / length;
  return true;
}

// Rob on another player: offered while the local player stands behind a replica.
void tick_pickpocket(void* entity_system, void* local_actor, void* local_entity) {
  // Rob looks this far for a victim, and from chest to chest for a clear line.
  constexpr float kRobLook = 3.0f;
  constexpr float kChest = 1.3f;
  kcds::runtime::pickpocket::Body local;
  local.actor = local_actor;
  // Nobody robs from a ladder (Rob was offered while climbing).
  const bool on_ladder =
      local_actor && slot<bool(__fastcall*)(void*)>(local_actor, kActorIsOnLadder)(local_actor);
  const bool allowed = local_actor && body_of(local_entity, local) && !local_rider_seated.load() &&
                       !game_menu_open() && !game_in_dialogue && !on_ladder;
  std::vector<kcds::runtime::pickpocket::Body> replicas;
  if (allowed) {
    for (const auto& [id, remote] : remotes) {
      // A knocked-out player is not robbed but searched (their body's Loot): Rob took the
      // body's prompt over. Nor is a player on a ladder.
      if (kcds::runtime::knockout::replica_down(std::uint32_t(id)) || ladder_climbers.count(id)) {
        continue;
      }
      kcds::runtime::pickpocket::Body body;
      body.player = id;
      body.actor = remote.actor;
      if (!body_of(get_entity(entity_system, remote.engine_id), body)) {
        continue;
      }
      const float dx = body.position[0] - local.position[0];
      const float dy = body.position[1] - local.position[1];
      if (dx * dx + dy * dy > kRobLook * kRobLook) {
        continue;
      }
      // Only with nothing solid between them (a player behind a wall was robbed).
      const float from[3] = {local.position[0], local.position[1], local.position[2] + kChest};
      const float to[3] = {body.position[0], body.position[1], body.position[2] + kChest};
      if (!kcds::runtime::physics::clear_between(from, to)) {
        continue;
      }
      replicas.push_back(body);
    }
  }
  kcds::runtime::pickpocket::tick(local, replicas, allowed);
}

void tick_pickpocket_orders(void* entity_system) {
  std::vector<kcd2s::integration::PickpocketRule> rules;
  std::vector<kcd2s::integration::PickpocketPockets> pockets;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    pockets = client->peer().take_pickpocket_pockets();
    rules = client->peer().take_pickpocket_rules();
  }
  // The victim's replica holds what the victim carries before the game opens its pockets.
  for (const auto& victim : pockets) {
    auto remote = remotes.find(victim.victim);
    if (remote != remotes.end()) {
      kcds::runtime::inventory::fill_pockets(get_entity(entity_system, remote->second.engine_id),
                                             remote->second.actor, victim.rows);
    }
  }
  for (const auto& rule : rules) {
    kcds::runtime::pickpocket::apply_rule(rule);
  }
}

}  // namespace kcds::game_client
