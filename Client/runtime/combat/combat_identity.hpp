// Who fights here: the replicas' combat souls and movements, the local player's and the
// replicas' combat targets and hard lock, a fight cancelled with an opponent, and the local
// fighter's combat actions relayed to the others. Much of its shared state is declared in
// core/client_internal.hpp.
#pragma once

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#include "kcd2s/core/ids.hpp"
#include "kcd2s/integration/combat.hpp"

namespace kcds::game_client {

// The native combat eligibility of two souls (hooked: the original).
using CombatEligibility = char(__fastcall*)(void*, void*, void*);
extern CombatEligibility original_combat_eligibility;
// The target the local player last asked to lock on, and the replays' scopes and records of
// attack inputs (engine thread).
extern std::atomic<kcd2s::core::EntityId> local_requested_target;
extern thread_local unsigned manual_combat_scope;
extern thread_local bool replaying_refused_press;
extern thread_local bool local_attack_made;
extern std::unordered_set<kcd2s::core::EntityId> refused_attack_inputs;
extern std::unordered_map<kcd2s::core::EntityId, const void*> dropped_attack_presses;
extern std::unordered_set<kcd2s::core::EntityId> riposte_attack_inputs;
// The Soul of the player with this network id here, and the IEntity of a player's Soul (the
// caller holds combat_identity_mutex).
void* combat_soul_of(kcd2s::core::EntityId id);
void* combat_entity_of(void* soul);

// The combat target setter (hooked), and the hard lock kept through a relayed hit.
using SetCombatTarget = std::intptr_t(__fastcall*)(void*, void*);
extern SetCombatTarget original_set_combat_target;
inline constexpr ULONGLONG kKeepLockMs = 900;
// The combat controller's hard-lock byte (movement+1104 +192).
inline constexpr std::size_t kHardLockFlag = 192;
extern std::atomic<bool> keep_hard_lock;
extern std::atomic<void*> keep_lock_entity;
extern std::atomic<ULONGLONG> keep_lock_until;
// The local player's last lock the game let go (its IEntity), when, and its hard-lock byte then:
// an NPC handed to this game is made anew, and the game drops the lock on the old body first
// (native_npcs.cpp relock).
extern std::atomic<void*> lost_lock_entity;
extern std::atomic<ULONGLONG> lost_lock_at;
extern std::atomic<std::uint8_t> lost_lock_hard;
std::intptr_t __fastcall hooked_set_combat_target(void* movement, void* entity);

// The relayed combat action of an input action name, and the local fighter's target relayed.
kcd2s::integration::CombatAction action_kind(const char* name);
void relay_native_target(void* actor);

}  // namespace kcds::game_client
