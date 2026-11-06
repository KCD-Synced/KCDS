// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "npc_blows.hpp"

#include "../combat/player_damage.hpp"
#include "../core/log.hpp"
#include "../knockout/knockout.hpp"
#include "native_npcs.hpp"
#include "npc_attack.hpp"
#include "npc_health.hpp"

#include <cstring>

namespace kcds::game_client {

namespace integration = kcd2s::integration;

// A blow or shot on a body of a server NPC (a HitInfo: the source's entity id at + 0,
// the target's at + 40, the health it takes at + 48, its type at + 24). The body takes none of
// the health, since every body shows the server's. The striker's own client, or the NPC's
// simulator for anything but a player, reports the health. On the simulator's body the hit lands
// and its reaction plays; a replica's is dropped (true): the simulator's reaction comes with its
// clips, and the replica playing its own as well jerked between the two.
bool take_npc_hit_health(const std::uint8_t* local, std::uint8_t* hit) {
  namespace npc_health = kcds::runtime::npcs::health;
  const auto target = *reinterpret_cast<const unsigned*>(hit + 40);
  const auto body = npc_health::body_of_entity(target);
  if (!body) {
    return false;
  }
  // Npc.attackable false: what is not another NPC's blow lands nowhere, so the NPC does not stagger
  // or turn on anyone, and the game sees no attack to punish.
  if (!npc_health::attackable(body->npc) &&
      !npc_health::body_of_entity(*reinterpret_cast<const unsigned*>(hit))) {
    return true;
  }
  float amount = 0;
  std::memcpy(&amount, hit + 48, sizeof(amount));
  const auto source_id = *reinterpret_cast<const unsigned*>(hit);
  auto source = npc_health::Source::World;
  std::uint32_t attacker = 0;
  if (local && source_id == *reinterpret_cast<const unsigned*>(local + 48)) {
    source = npc_health::Source::LocalPlayer;
    attacker = local_network_id.load();
    // The NPC this client simulates may fight the local player back (npc_attack.hpp struck_by):
    // for a combat blow, not a collision or a push, and never while the player lies knocked out
    // (the falling body's contacts made the bandit take them on again right after the
    // revive).
    void* soul = *reinterpret_cast<void* const*>(local + 1640);
    const bool struck =
        kcds::runtime::npcs::attack::fight_back_hit(hit) && !kcds::runtime::knockout::knocked_out();
    if (body->simulated && soul && struck) {
      kcds::runtime::npcs::attack::struck_by(body->npc, *reinterpret_cast<const std::uint64_t*>(
                                                            static_cast<std::uint8_t*>(soul) + 48));
      // Fighting another, the NPC turns on the local player.
      note_player_strike(body->npc, attacker);
    }
  } else if (const auto striker = npc_health::body_of_entity(source_id)) {
    source = npc_health::Source::Npc;
    attacker = striker->npc;
  } else if (hit_attacker(hit) != 0) {
    source = npc_health::Source::RemotePlayer;
  }
  constexpr unsigned kCollisionHitType = 3;
  constexpr std::uint8_t kCombat = 1;
  constexpr std::uint8_t kCollision = 4;
  const bool collision = *reinterpret_cast<const unsigned*>(hit + 24) == kCollisionHitType;
  // Where the blow landed, as the game priced it; none for a collision.
  const std::int32_t subpart = collision ? -1 : take_priced_npc_subpart(body->npc);
  const auto report = npc_health::hit_to_report(*body, source, attacker, amount,
                                                collision ? kCollision : kCombat, subpart);
  if (report) {
    npc_health::queue_hit(*report);
  }
  // The local player's blow on a replica: the simulator's game plays it on the NPC's body too,
  // so the NPC reacts and its AI learns who struck it. Walking into the body is a collision or a
  // push, no blow: sent, it made the server give the player a fight with the NPC and its
  // simulation, and the NPC's body was made again under the player (players walked
  // through the level's people, who jumped).
  if (source == npc_health::Source::LocalPlayer && !body->simulated &&
      kcds::runtime::npcs::attack::fight_back_hit(hit)) {
    integration::NpcBlow blow;
    blow.npc = body->npc;
    blow.player = attacker;
    blow.direction = integration::NpcBlowDirection::PlayerStruckNpc;
    blow.hit = integration::portable_npc_hit(hit);
    npc_health::queue_blow(blow);
  }
  const float none = 0;
  std::memcpy(hit + 48, &none, sizeof(none));
  void* npc_actor = body->simulated ? server_npc_actor(body->npc) : nullptr;
  const auto combat = npc_actor ? kcds::runtime::npcs::attack::combat_snapshot(npc_actor)
                                : kcds::runtime::npcs::attack::CombatSnapshot{};
  log("NPC hit npc=%u source=%u kind=%d health=%.2f reported=%u defending=%u between swings=%u "
      "combat state=%d block=%d",
      body->npc, source_id, int(source), amount, unsigned(report.has_value()),
      unsigned(npc_actor && kcds::runtime::npcs::attack::defending(npc_actor)),
      unsigned(npc_actor && kcds::runtime::npcs::attack::ready_to_swing(npc_actor)), combat.state,
      combat.block);
  return !body->simulated;
}

void scale_npc_blow_on_local(const std::uint8_t* local, std::uint8_t* hit) {
  namespace npc_health = kcds::runtime::npcs::health;
  if (!local || !hit) {
    return;
  }
  const auto target = *reinterpret_cast<const unsigned*>(hit + 40);
  if (target != *reinterpret_cast<const unsigned*>(local + 48)) {
    return;
  }
  const auto striker = npc_health::body_of_entity(*reinterpret_cast<const unsigned*>(hit));
  if (!striker) {
    return;
  }
  const float share = npc_health::damage_share(striker->npc);
  if (share == integration::kNpcDamageGame) {
    return;
  }
  float amount = 0;
  std::memcpy(&amount, hit + 48, sizeof(amount));
  const float scaled = amount * share;
  std::memcpy(hit + 48, &scaled, sizeof(scaled));
  log("NPC blow npc=%u on the local player: %.2f of %.2f (share %.2f)", striker->npc, scaled,
      amount, share);
}

// A blow of an NPC this client simulates on another player's replica: the NPC's
// attack plays only in this game, so the struck player's own game takes the blow (NPC_BLOW).
void relay_npc_blow(const std::uint8_t* hit) {
  namespace npc_health = kcds::runtime::npcs::health;
  const auto striker = npc_health::body_of_entity(*reinterpret_cast<const unsigned*>(hit));
  if (!striker || !striker->simulated) {
    return;
  }
  const auto target = *reinterpret_cast<const unsigned*>(hit + 40);
  core::EntityId victim = 0;
  {
    std::lock_guard<std::mutex> lock(combat_identity_mutex);
    for (const auto& entry : remote_combat_movements) {
      auto actor =
          *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(entry.second) + 504);
      if (actor && *reinterpret_cast<const unsigned*>(actor + 48) == target) {
        auto identity = remote_combat_souls.find(entry.first);
        if (identity != remote_combat_souls.end()) {
          victim = identity->second;
        }
        break;
      }
    }
  }
  if (!victim) {
    return;
  }
  integration::NpcBlow blow;
  blow.npc = striker->npc;
  blow.player = victim;
  blow.direction = integration::NpcBlowDirection::NpcStruckPlayer;
  blow.hit = integration::portable_npc_hit(hit);
  npc_health::queue_blow(blow);
  float health = 0;
  std::memcpy(&health, hit + 48, sizeof(health));
  log("NPC blow npc=%u struck player=%u health=%.2f", blow.npc, victim, health);
}

}  // namespace kcds::game_client
