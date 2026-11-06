// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "body_containers.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "../core/natives.hpp"
#include "../npcs/native_npcs.hpp"
#include "../pickpocket/pickpocket.hpp"
#include "../props/prop_entity.hpp"
#include "../replicas/remote.hpp"
#include "stash_tick.hpp"
#include "stashes.hpp"

namespace kcds::game_client {

// How far above a cart's root part its storage's container stands (metres).
constexpr float kCartStorageHeight = 0.6f;

void tick_body_containers(const Frame& frame) {
  const auto& entities = frame.entities;
  const auto entity_system = frame.entity_system;
  // A knocked-out player's body is their container: it stands where this game shows the body,
  // wherever it was carried. So is a dead NPC's, where its body lies here.
  // A dead NPC's body also opens it with the game's own Rob, and keeps its clothes on; no other
  // body of the server's NPCs is robbed.
  // Nor is one pickpocketed with the game's own pickpocketing.
  {
    std::vector<std::uint32_t> server_bodies;
    std::vector<void*> server_actors;
    for (const auto& [id, npc] : native_npcs) {
      server_bodies.push_back(npc.engine_id);
      server_actors.push_back(npc.actor);
    }
    kcds::runtime::stash::set_server_bodies(std::move(server_bodies));
    kcds::runtime::pickpocket::set_unrobbable(std::move(server_actors));
  }
  tick_stashes(entities, [&](core::EntityId owner, kcds::runtime::stash::Seen& container) {
    float* at = container.position;
    const auto remote = remotes.find(owner);
    void* body =
        remote != remotes.end() ? get_entity(entity_system, remote->second.engine_id) : nullptr;
    if (const auto npc = native_npcs.find(owner); !body && npc != native_npcs.end()) {
      body = get_entity(entity_system, npc->second.engine_id);
      container.body_entity = npc->second.engine_id;
      container.wearer = npc->second.actor;
    }
    if (!body) {
      // A cart's storage (its bodyOf names the cart): on the cart's root part, where this game
      // shows the cart, a little above it so the game's Loot prompt is at the bed.
      void* cart = kcds::runtime::props::entity::find("kcds_cart_" + std::to_string(owner));
      if (!cart) {
        return false;
      }
      kcds::runtime::props::entity::world_position(cart, at);
      at[2] += kCartStorageHeight;
      return true;
    }
    kcds::runtime::props::entity::world_position(body, at);
    return true;
  });
}

}  // namespace kcds::game_client
