// The local player's stats, read off its soul through the game's own
// natives and published in the NetworkPlayer fields "vitals", "stats" and "abilities"
// (kcd2s/integration/player_stats.hpp), and the server's Player.setStat, fillStat and addStat
// applied to it.
//
// The soul (actor + 1640, WHGame 1.5.5):
//
//   soul + 1800 + 4 * state   its states: 0 health, 1 stamina, 2 exhaust, 3 hunger (the cache the
//                             game's own code reads)
//                 a derived stat (soul, id, context): the game's GetDerivedStat, with
//                             the ids of its derived-stat table (rpg/concept definitions)
//                 an attribute's relative level (soul, stat, context): the soul's
//                             vtable slot 88, GetStatLevelRelative, the level within 0 and the
//                             highest level divided by the highest level
//                 a state's maximum (soul, state): the one the setter clamps to (the
//                             RPG parameters' for health, exhaust and hunger, the derived max
//                             stamina for stamina)
//                 an attribute as the game evaluates it (soul, stat, context): its
//                             level with every modifier, the script binding's GetStatLevel
//                 a skill as the game evaluates it (soul, skill, context), within 0
//                             and the highest skill level (the RPG parameters' + 180): the
//                             script binding's GetSkillLevel
//                 a state's setter (soul, state, value): the script binding's
//                             SetState, which clamps the value to the state's maximum and applies
//                             it through the game's RPG events; it leaves health alone at 0
//
// Health 0 is the game's own damage instead (the soul's damage interface at soul + 240, vtable
// slot 27, as the script's soul:DealDamage calls it): the knock-out that follows a lethal hit in a
// server world (Client/runtime/knockout) reports it as playerDied.
//
// Engine thread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "kcd2s/integration/player_stats.hpp"

namespace kcds::runtime::stats {

using Logger = void (*)(const char* format, ...);
using HealthReduction = void (*)(float amount);

struct Natives {
  void* derived_stat = nullptr;         // GetDerivedStat
  void* stat_level_relative = nullptr;  // GetStatLevelRelative
  void* set_state = nullptr;            // SetState
  void* state_max = nullptr;            // a state's maximum
  void* stat_level = nullptr;           // GetStatLevel
  void* skill_level = nullptr;          // GetSkillLevel
  void* rpg_params = nullptr;           // the RPG parameters
  void* damage_type_init = nullptr;     // a default damage type
  void* damage_type_free = nullptr;     // its release
};

// False when a native is missing.
bool install(const Natives& natives, Logger logger);

// The fields the local player publishes.
struct Fields {
  std::string vitals;
  std::string stats;
  std::string abilities;
};

// Every frame: reads the local player's soul (nullptr outside a server world: the fields are
// emptied) at now_ms. Health and conditions are published as they change; stamina, exhaustion and
// hunger at most every 100 ms; the other derived stats, attributes and abilities every 250 ms.
const Fields& capture(void* soul, std::uint64_t now_ms);

// Applies the server's setStat requests to the local player's soul. Requests that arrive while it
// lies knocked out belong to a life that has ended and are dropped. An addStat request (relative)
// adds its change to the state's current value, within 0 and the state's maximum, and is then
// applied as a setStat of that value (health 0: the game's own lethal damage).
// After each positive health setter, health_removed synchronously receives the actual health
// removed by that native call. Zero-health requests use native damage and are not reported here.
// The game's own lethal damage on a soul: in a server world a knock-out follows.
void deal_lethal(void* soul);

void apply(void* soul, const std::vector<kcd2s::integration::StatRequest>& requests,
           bool knocked_out, HealthReduction health_removed = nullptr);

// Keeps a replica's health, stamina and exhaustion at its owner's, through the
// game's own state setter. The replica's replayed swings and blocks drained its own stamina, and a
// tired replica kept opening its mouth for breath while its owner was rested. Its health differed
// on every screen (87 on the attacker's, 75 on its own, 100 on a watcher's), so each game
// picked other hit reactions and master-strike variants (a lethal one on the striker's screen
// only). Health 0 (a knock-out) is left to the knock-out sync.
// Health reconciles to the wire's hundredth-unit precision; stamina and exhaustion retain a
// one-unit deadband to avoid repeatedly correcting the replica's own per-frame changes.
void keep_replica_vitals(void* soul, const kcd2s::integration::PlayerVitals& owner);

}  // namespace kcds::runtime::stats
