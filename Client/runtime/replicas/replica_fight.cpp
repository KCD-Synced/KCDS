// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "replica_fight.hpp"

#include "../core/log.hpp"
#include "../combat/sync_attacks.hpp"
#include "../core/natives.hpp"
#include "remote.hpp"

#include <unordered_map>

namespace kcds::game_client {

// A replica whose owner is locked on joins the fight with the game's own lock-on press.
// The owner relays its lock target (Target, TargetOther), but a lock the game takes by itself (no
// lock key) sends no press, and a replica that left the fight (a knock-out, a revive) stayed out
// of it: live, the owner stood in guard while its replica idled for minutes on the opponent's
// screen, where every relayed master strike was refused (no combat action). The registry entry is
// the lock action's (Lock, + 312 in the replayed inputs' registry offsets).
constexpr unsigned kNoNativeAction = 28;
constexpr std::size_t kLockInputEntry = 312;
constexpr std::size_t kMovementController = 1104;
constexpr std::size_t kControllerInput = 40;
void replay_replica_lock(void* movement, void* soul, std::uint8_t mode) {
  auto controller =
      *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement) + kMovementController);
  auto native_input =
      controller ? *reinterpret_cast<void**>(controller + kControllerInput) : nullptr;
  auto registry =
      *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(get_services()) + 48);
  if (!native_input || !registry) {
    return;
  }
  replaying_combat_input = true;
  auto previous_source = replayed_combat_movement;
  replayed_combat_movement = movement;
  auto previous_intent = remote_manual_combat_soul;
  remote_manual_combat_soul = soul;
  original_combat_input(native_input, registry + kLockInputEntry, mode, mode == 1 ? 1.0f : 0.0f);
  remote_manual_combat_soul = previous_intent;
  replayed_combat_movement = previous_source;
  replaying_combat_input = false;
}

// Every frame, per replica: a replica out of the fight while its owner is locked on (the owner's
// relayed target) joins it again after a short moment, unless it lies knocked out. The press is
// held like a relayed one (press and release in one moment never locked on), and given up after
// a few tries until the replica or its owner changes: each press cut the replica's animation
// (a press every 600 ms that never locked on). Engine thread.
std::unordered_map<core::EntityId, ReplicaRejoin> replica_out_of_fight_since;
constexpr ULONGLONG kReplicaRejoinMs = 600;
constexpr ULONGLONG kReplicaLockHoldMs = 100;
constexpr unsigned kReplicaRejoinTries = 2;
void keep_replica_in_fight(core::EntityId player, void* actor, void* movement) {
  void* soul =
      actor ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + 1640) : nullptr;
  core::EntityId target = 0;
  {
    std::lock_guard<std::mutex> lock(combat_identity_mutex);
    auto selection = remote_native_targets.find(soul);
    if (selection != remote_native_targets.end()) {
      target = selection->second;
    }
  }
  const ULONGLONG now = GetTickCount64();
  auto entry = replica_out_of_fight_since.find(player);
  // A held press is always released, whatever happened meanwhile.
  if (entry != replica_out_of_fight_since.end() && entry->second.pressed_at &&
      now - entry->second.pressed_at >= kReplicaLockHoldMs && movement) {
    replay_replica_lock(movement, soul, 2);
    entry->second.pressed_at = 0;
    entry->second.out_since = now;
  }
  if (!movement || !target || native_action_type(movement) != kNoNativeAction ||
      kcds::runtime::knockout::replica_down(player)) {
    if (entry != replica_out_of_fight_since.end() && !entry->second.pressed_at) {
      replica_out_of_fight_since.erase(entry);
    }
    return;
  }
  if (entry == replica_out_of_fight_since.end()) {
    entry = replica_out_of_fight_since.emplace(player, ReplicaRejoin{now, 0, 0}).first;
  }
  ReplicaRejoin& rejoin = entry->second;
  if (rejoin.pressed_at || rejoin.tries >= kReplicaRejoinTries ||
      now - rejoin.out_since < kReplicaRejoinMs) {
    return;
  }
  replay_replica_lock(movement, soul, 1);
  rejoin.pressed_at = now;
  ++rejoin.tries;
  log("COMBAT replica player=%u joins its owner's fight (lock on %u, try %u)", player, target,
      rejoin.tries);
}

}  // namespace kcds::game_client
