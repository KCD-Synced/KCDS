// Defence on every screen: the owner's perfect block, master strike and paired perfect block
// started natively on its replicas, the combat-model values (stance, guard, mode) kept on them,
// and the pair diagnostics.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "../core/client_internal.hpp"

namespace kcds::game_client {

// The combat stance, the same kind of property: a pair swaps both fighters' (reads
// it for the pair's rows).
constexpr std::size_t kCoreStance = 192;
constexpr unsigned kMaxGuardZone = 15;
constexpr unsigned kMaxDefenceMode = 255;
// The combat-model properties CombatModel carries, by its mode - 1: the stance, the guard zone and
// the value at core + 96.
constexpr std::array<std::size_t, 3> kCoreModelProperties = {kCoreStance, kCoreGuard, kCoreMode};
// The first two are kept on a replica every frame; the third only at a relayed pair start.
constexpr std::size_t kKeptModelProperties = 2;
constexpr char kPerfectBlockKind = 1;
constexpr unsigned kNativePerfectBlockAction = 7;
// The native action of a fighter taking a hit (its hit reaction).
constexpr unsigned kNativeHitAction = 5;
// The master strike: a player's C_CombatMultiTriggerMasterStrike (the defender's trigger 8, kind
// 5) fires through into kind 5 while the window an attack's
// Start schedules for it (C_CombatActorSlotPlanner,with 4, 10 and 8) is open; in a
// game alone it played SYNC_PERFECT_BLOCK (9) and the attacker SYNC_PERFECT_BLOCK_HIT (10).
constexpr char kMasterStrikeKind = 5;
// A perfect block of a paired (sync) attack (the defender's trigger 6): the same
// SYNC_PERFECT_BLOCK, the attacker's SyncAttack (22) turned into SYNC_PERFECT_BLOCK_HIT.
constexpr char kPairPerfectBlockKind = 4;
constexpr unsigned kNativeMasterStrikeAction = 9;
constexpr unsigned kNativeMasterStruckAction = 10;
// The start's request is the input that made it (passes the pressed input's id to):
// the pair's animation rows are found by it (query + 124), so the
// replica's start carries the owner's (the perfect block's request found no row). The
// relayed value for the game's "none" (0xFFFFFFFF).
constexpr unsigned kNoMasterStrikeRequest = 255;

// The defence start (hooked: the original) and the perfect block's request.
using StartDefence = void**(__fastcall*)(void* movement, void** action, char kind, unsigned variant,
                                         char flag, unsigned request);
extern StartDefence start_defence;
extern StartDefence original_start_defence;
extern const unsigned* perfect_block_request;
void** __fastcall hooked_start_defence(void* movement, void** action, char kind, unsigned variant,
                                       char flag, unsigned request);
// A combat model's int property set as the game sets it.
using SetCombatIntProperty = void(__fastcall*)(void* property, int value);
extern SetCombatIntProperty set_combat_int_property;
extern std::unordered_map<core::EntityId, unsigned> relayed_defence_guards;
extern std::unordered_map<core::EntityId, unsigned> relayed_defence_modes;
extern std::unordered_map<core::EntityId, std::array<unsigned, 3>> owner_models;
struct PendingPerfectBlock {
  void* movement = nullptr;
  unsigned variant = 0;
  ULONGLONG until = 0;
  // kPerfectBlockKind, kMasterStrikeKind or kPairPerfectBlockKind.
  char kind = kPerfectBlockKind;
  // The start's request: the block trigger's for a perfect block, the owner's for a master strike.
  unsigned request = 0;
  // The owner's guard and core + 96 value at its start (DefenceGuard, DefenceMode) for a pair
  // start, ~0 for none.
  unsigned guard = ~0u;
  unsigned mode = ~0u;
};
// The owners' perfect blocks waiting for their replicas' block (engine thread).
extern std::unordered_map<core::EntityId, PendingPerfectBlock> pending_perfect_blocks;
void tick_pending_perfect_blocks();
void start_replica_perfect_block(core::EntityId player, void* movement, unsigned variant, char kind,
                                 unsigned request);
// The owner's combat-model value taken on its replica, and the local fighter's relayed.
void take_owner_property(void* movement, std::size_t property, unsigned value);
void relay_combat_model(void* actor);
// The local game made a SYNC_PERFECT_BLOCK during the input being handled, and the attack press it
// turned into the master strike was withheld (engine thread).
extern bool input_made_sync_perfect_block;
extern bool attack_press_withheld;
// The perfect-block window lookup (hooked) and the quick-event predicate (observed).
using WindowLookup = void*(__fastcall*)(void* trigger, int attacker);
extern WindowLookup original_window_lookup;
void* __fastcall hooked_window_lookup(void* trigger, int attacker);
using PerfectBlockWindow = bool(__fastcall*)(void*);
extern PerfectBlockWindow original_perfect_block_window;
bool __fastcall hooked_perfect_block_window(void* trigger);

}  // namespace kcds::game_client
