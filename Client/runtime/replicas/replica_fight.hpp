// A replica whose owner is locked on joins the fight with the game's own lock-on press.
#pragma once

#include <Windows.h>

#include <cstdint>
#include <unordered_map>

#include "kcd2s/core/ids.hpp"

namespace kcds::game_client {

// A replica out of the fight while its owner is locked on: since when, the held lock press and
// the tries.
struct ReplicaRejoin {
  ULONGLONG out_since = 0;
  ULONGLONG pressed_at = 0;  // the lock is held since then; 0 when released
  unsigned tries = 0;
};
extern std::unordered_map<kcd2s::core::EntityId, ReplicaRejoin> replica_out_of_fight_since;
void replay_replica_lock(void* movement, void* soul, std::uint8_t mode);
void keep_replica_in_fight(kcd2s::core::EntityId player, void* actor, void* movement);

}  // namespace kcds::game_client
