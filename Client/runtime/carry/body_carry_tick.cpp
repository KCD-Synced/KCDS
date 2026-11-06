#include "body_carry_tick.hpp"

#include "../action_hints/resource_action_hints.hpp"
#include "../appearance/components.hpp"
#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../ground/ground_items.hpp"
#include "../knockout/knockout.hpp"
#include "../pickpocket/pickpocket_tick.hpp"
#include "../replicas/remote.hpp"
#include "body_carry.hpp"
#include "carry_items.hpp"

#include <cmath>
#include <vector>

namespace kcds::game_client {

// Players' bodies carried on the shoulder: the server's carries on this game's
// bodies, and the local player's own pick-ups and let-gos to the server.
void tick_body_carry(void* entity_system, void* local_actor, void* local_entity) {
  std::vector<kcd2s::integration::BodyCarry> carries;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    carries = client->peer().take_body_carries();
  }
  const std::uint32_t local_entity_id =
      local_entity
          ? *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(local_entity) + 12)
          : 0;
  kcds::runtime::carry::Players players;
  players.local = std::uint32_t(local_network_id.load());
  players.local_actor = local_actor;
  players.body_of = [&](std::uint32_t player) {
    kcds::runtime::carry::Body body;
    if (player == players.local) {
      body.actor = local_actor;
      body.entity = local_entity_id;
    } else if (auto it = remotes.find(player); it != remotes.end()) {
      body.actor = it->second.actor;
      body.entity = it->second.engine_id;
    }
    return body;
  };
  players.player_of = [&](std::uint32_t entity) -> std::uint32_t {
    for (const auto& [id, remote] : remotes) {
      if (remote.engine_id == entity) {
        return std::uint32_t(id);
      }
    }
    return 0;
  };
  // The game's own "Grab" (BasicAIActions: @ui_hud_grab on the grab_body action, our key
  // attack_special, which calls RequestGrabCorpse) over a knocked-out player's replica within
  // reach, which the game does not offer on a player replica by itself (no AI actions).
  constexpr std::int64_t kGrabHint = -51;
  // The body's root (its hips) lies about 1.9 m from a player standing at its feet.
  constexpr float kGrabReach = 2.2f;
  static bool grab_offered = false;
  std::uint32_t nearest_entity = 0;
  kcds::runtime::pickpocket::Body local_body;
  // The game offers the grab only to men (BasicAIActions: not IsFemale); the gender is read
  // again once a second.
  static bool local_female = false;
  static double next_gender_read = 0;
  const double now_seconds = local::clock_seconds();
  if (local_actor && now_seconds >= next_gender_read) {
    next_gender_read = now_seconds + 1;
    local_female = kcds::runtime::appearance::components::worn(local_actor).gender == "female";
  }
  if (local_actor && !local_female && kcds::runtime::carry::carried_entity(local_actor) == 0 &&
      body_of(local_entity, local_body)) {
    float best = kGrabReach;
    for (const auto& [id, remote] : remotes) {
      kcds::runtime::pickpocket::Body body;
      // A fainted player is not knocked out: the server refuses its carry.
      if (!kcds::runtime::knockout::replica_down(std::uint32_t(id)) ||
          kcds::runtime::knockout::replica_fainted(std::uint32_t(id)) ||
          kcds::runtime::carry::body_held(std::uint32_t(id), remote.engine_id, local_actor) ||
          !body_of(get_entity(entity_system, remote.engine_id), body)) {
        continue;
      }
      const float dx = body.position[0] - local_body.position[0];
      const float dy = body.position[1] - local_body.position[1];
      const float distance = std::sqrt(dx * dx + dy * dy);
      if (distance <= best && std::fabs(body.position[2] - local_body.position[2]) < 1.5f) {
        best = distance;
        nearest_entity = remote.engine_id;
      }
    }
  }
  if (nearest_entity) {
    kcds::runtime::action_hints::EngineHint hint;
    hint.id = kGrabHint;
    hint.key = "attack_special";
    hint.text = "@ui_hud_grab";
    hint.localized = true;
    hint.on_fire = [local_actor, nearest_entity] {
      kcds::runtime::carry::pick_up(local_actor, nearest_entity);
    };
    kcds::runtime::action_hints::set_engine_hint(hint);
    grab_offered = true;
  } else if (grab_offered) {
    kcds::runtime::action_hints::clear_engine_hint(kGrabHint);
    grab_offered = false;
  }
  for (const auto& report : kcds::runtime::carry::tick(carries, players)) {
    log("CARRY local player %s the body of player=%u",
        report.kind == kcd2s::integration::BodyCarryReport::Kind::Started ? "picked up"
                                                                          : "let go of",
        report.carried);
    std::lock_guard<std::mutex> lock(network_mutex);
    client->peer().send_body_carry_report(report);
  }
}

// Where an item lying in the world is (its entity's position), for a carry's put-down.
bool carried_item_position(void* item, float* position) {
  void* framework = framework_slot ? *framework_slot : nullptr;
  auto actors =
      framework ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(framework) + 1304) : nullptr;
  void* entities =
      actors ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actors) + 48) : nullptr;
  const std::uint32_t id = kcds::runtime::ground::entity_of(item);
  void* entity = entities && id ? get_entity(entities, id) : nullptr;
  const float* tm = entity ? slot<const float*(__fastcall*)(void*)>(entity, 32)(entity) : nullptr;
  if (!tm) {
    return false;
  }
  position[0] = tm[3];
  position[1] = tm[7];
  position[2] = tm[11];
  return true;
}

// Tells the server where the local player's carry let its item go (CARRY_REPORT).
void report_carry_put_down(const std::array<std::uint8_t, 16>& item_class, const float* position) {
  kcd2s::integration::CarryPutDown put_down;
  put_down.item = item_class;
  std::memcpy(put_down.position, position, sizeof(put_down.position));
  std::lock_guard<std::mutex> lock(network_mutex);
  client->peer().send_carry_put_down(put_down);
}

}  // namespace kcds::game_client
