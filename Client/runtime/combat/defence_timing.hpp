// A defender's block on its replica: the defender's own timing against the running attack,
// and the replica's catch-up so its guard stands where its owner's stood against the swing.
#pragma once

#include <Windows.h>

#include <cstdint>
#include <unordered_map>

#include "kcd2s/core/ids.hpp"

namespace kcds::game_client {

// When the local player's block came after the running replica attack started, -1 for none.
std::int64_t defence_timing_ms();
// A player's defence timing: ms after the attack started on their screen, when it arrived.
struct DefenceTiming {
  ULONGLONG after_attack = 0;
  ULONGLONG received = 0;
};
extern std::unordered_map<kcd2s::core::EntityId, DefenceTiming> defence_timings;
// The entity of the replica attacking now (for the defender's target), null for none.
void* running_attacker_entity(kcd2s::core::EntityId player);
// The last measured contact-to-answer time of a local swing, in ms.
extern ULONGLONG combat_round_trip_ms;
void start_defence_catch_up(kcd2s::core::EntityId player);
void tick_defence_catch_ups();

}  // namespace kcds::game_client
