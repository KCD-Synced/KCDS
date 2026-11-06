// Paired (sync) attacks chosen by the attacker's owner: the native databases and factories,
// the owner's pair selection applied on this screen, and the replicas' own choice left out.
// remote_combat_id is declared in core/client_internal.hpp.
#pragma once

#include <Windows.h>

#include <cstdint>
#include <unordered_map>

#include "kcd2s/core/ids.hpp"
#include "kcd2s/integration/combat.hpp"

namespace kcds::game_client {

// the actor action query queue (hooked: logged for fighters).
using QueueCombatQuery = std::intptr_t(__fastcall*)(void*, std::uint8_t, const void*);
extern QueueCombatQuery original_queue_combat_query;
std::intptr_t __fastcall hooked_queue_combat_query(void* self, std::uint8_t kind,
                                                   const void* query);

// The native SyncHit and SyncAttack row databases, and the owners' pair selections waiting here.
extern std::uint8_t* native_sync_hit_database;
extern std::uint8_t* native_sync_attack_database;
struct PendingSyncHit {
  kcd2s::integration::NativeSyncHitData data;
  ULONGLONG received;
};
extern std::unordered_map<kcd2s::core::EntityId, PendingSyncHit> pending_sync_hits;
// The replica movement whose replayed combat input runs now (engine thread).
extern thread_local void* replayed_combat_movement;
void* find_native_sync_hit(const kcd2s::integration::NativeSyncHitData& data);
void* find_native_sync_attack(const kcd2s::integration::NativeSyncHitData& data);

// The SyncHit factory, the SyncAttack choice and construction, and the attack factory.
using MakeSyncHit = void*(__fastcall*)(void*, void**, void*, bool, int);
extern MakeSyncHit original_make_sync_hit;
void* __fastcall hooked_make_sync_hit(void* factory, void** out, void* row, bool flags,
                                      int context);
using ChooseSyncAttack = void*(__fastcall*)(void*, void**, const void*);
using ConstructSyncAttack = void*(__fastcall*)(void**, void*);
using MakeAttack = void*(__fastcall*)(void*, void**, unsigned, unsigned);
extern ChooseSyncAttack original_choose_sync_attack;
extern ConstructSyncAttack construct_sync_attack;
extern MakeAttack original_make_attack;
void* __fastcall hooked_choose_sync_attack(void* factory, void** out, const void* query);

}  // namespace kcds::game_client
