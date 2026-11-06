// Melee contacts between players: the game's own melee hit events, the local swing and
// its contacts relayed to the struck player's game, the relayed contacts and hit actions started
// on replicas, and the physical contact listeners.
#pragma once

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "../../../Backend/integration/local/native_event_relay.hpp"
#include "../../../Backend/integration/local/runtime.hpp"
#include "kcd2s/integration/combat.hpp"
#include "replica_contact.hpp"

namespace kcds::game_client {

namespace core = kcd2s::core;
namespace integration = kcd2s::integration;
namespace local = kcd2s::local;

using MeleeHit = void*(__fastcall*)(void*, void**, const void*);
using SubmitRpgEvent = char(__fastcall*)(void*, void*);
using DestroyRpgEvent = void*(__fastcall*)(void*, char);
using ModifyNativeEvent = void(__fastcall*)(void*);
using HitReaction = std::intptr_t(__fastcall*)(void*, const void*, void*, const void*);
using ProduceMelee = char(__fastcall*)(void*, void*, const void*, void*);
using ProduceNativeHit = char(__fastcall*)(void*, void*, float*, char*, const void*, bool, unsigned,
                                           void*);
using CompletePhysicalAttackPower = void(__fastcall*)(void*, float);
using SetNativeContactTarget = std::intptr_t(__fastcall*)(void*, char);
using MasterRiposteStart = std::intptr_t(__fastcall*)(void*);
// An attack the same action makes this soon after the swing started is still that swing, in ms.
constexpr ULONGLONG kSameSwingMs = 400;
struct SwingHold {
  void* skeleton = nullptr;
  core::EntityId victim = 0;
  ULONGLONG started = 0;
  ULONGLONG until = 0;
  float scales[16] = {};
  // The victim blocked or parried: the swing goes on when the deflection clip starts.
  bool awaiting_deflection = false;
};
// The longest a defended swing waits for its deflection clip, in ms (310-390 ms).
constexpr ULONGLONG kDeflectionWaitMs = 600;
// The native actions of a fighter who defends: 6 while the block is held (the guard a replayed
// block press puts a replica in), 7 the block or parry itself (the defender's own game reports it
// at the contact). The attack is 3 (kNativeAttackAction, core/client_internal.hpp).
constexpr unsigned kNativeGuardAction = 6;
constexpr unsigned kNativeDefenceAction = 7;
using PhysicalContact = void(__fastcall*)(void* listener, void* attack);
using CreateHitAction = void**(__fastcall*)(const void* caller, void** action, void* victim,
                                            int variant, const void* hit);
using PushCombatAction = char(__fastcall*)(void* movement, void** action);
using HitActionVariant = int(__fastcall*)(void* unused, unsigned strength, char standard);
using ActionHandlesHit = char(__fastcall*)(void* movement);
struct ReplicaHit {
  ULONGLONG marked = 0;
  ULONGLONG reacted = 0;
  integration::NativeReactionData data{};
  // From the reaction: the attacker and both movements here.
  core::EntityId attacker = 0;
  void* source_move = nullptr;
  void* victim_move = nullptr;
};
constexpr std::size_t kAttackRecordBytes = 512;
constexpr std::size_t kAttackVictimMovement = 200;

// The game's melee hit, its RPG event submit, destroy and modifiers (hooked: the originals), and
// the melee events waiting for their submit.
extern MeleeHit original_melee_hit;
extern void** rpg_module_slot;
extern std::mutex pending_melee_mutex;
extern local::NativeEventRelay pending_melee;
extern SubmitRpgEvent original_submit_rpg_event;
extern DestroyRpgEvent original_destroy_rpg_event;
extern ModifyNativeEvent original_modify_native_event;
// The committed melee event being replayed now (its modifiers are not run again).
extern thread_local void* committed_replay_event;
bool resolve_committed_equipment(integration::NativeMeleeData& cause, void* source_actor,
                                 void* target_actor);
void* __fastcall hooked_melee_hit(void* self, void** event, const void* data);
char __fastcall hooked_submit_rpg_event(void* self, void* event);
void* __fastcall hooked_destroy_rpg_event(void* self, char flags);
void __fastcall hooked_modify_native_event(void* event);

// The combat module's hit reaction (hooked), and the melee production with its native contact.
extern HitReaction original_hit_reaction;
std::intptr_t __fastcall hooked_hit_reaction(void* source, const void* info, void* target,
                                             const void* outcome);
extern ProduceMelee original_produce_melee;
extern ProduceNativeHit produce_native_hit;
extern CompletePhysicalAttackPower complete_physical_attack_power;
extern SetNativeContactTarget set_native_contact_target;
extern const unsigned* native_backoff_attack_type;
char __fastcall hooked_produce_melee(void* self, void* info, const void* context, void* outcome);
void prepare_native_contact_target(void* expected_core, void* contacted_movement);
void finish_native_physical_contact(void* movement, void* expected_core, float power,
                                    char native_flag);

// The local swing: its contacts by opponent, the running attack, its hold at the contact.
extern std::unordered_map<core::EntityId, std::uint64_t> contacted_attack_generation;
extern void* local_attack_action;
extern SwingHold swing_hold;
void advance_local_contact_generation();
void retire_local_swing(core::EntityId opponent);
void begin_local_combat_contact(void* action);
// The game started the local attack (its Start): the swing's own contacts count from now on.
void local_swing_start(void* action);
void hold_local_swing(core::EntityId victim);
void release_local_swing(bool answered);
// Called by frame() every frame: a held swing released once its victim answered or took too long.
void tick_swing_hold();
void local_clip_started(void* skeleton, int id, unsigned layer);
extern MasterRiposteStart original_master_riposte_start;
std::intptr_t __fastcall hooked_master_riposte_start(void* action);

// The replicas' contacts and UI blocking, and the owner's contacts relayed natively here.
extern std::unordered_map<core::EntityId, kcds::runtime::combat::ReplicaContact> replica_contacts;
std::optional<kcds::runtime::combat::ReplicaContactScope> replica_contact_scope(
    core::EntityId player, void* actor, void* movement);
extern std::unordered_set<core::EntityId> remote_ui_blocking;
extern std::atomic<std::uint64_t> local_ui_blocking_ms;
char relay_owned_native_contact(void* self, void* info, float* native_power, char* native_flag,
                                const void* context, bool commit, unsigned native_state,
                                void* outcome);

// The native hit action of a replica: its creation and push, and the pairs waiting here.
extern PhysicalContact native_physical_contact;
extern CreateHitAction create_hit_action;
extern PushCombatAction push_combat_action;
extern HitActionVariant hit_action_variant;
extern ActionHandlesHit action_handles_hit;
bool start_native_hit_action(void* source, void* victim, const std::uint8_t* hit, unsigned stance);
extern std::unordered_map<core::EntityId, ReplicaHit> replica_hits;
void give_replica_hit_action(core::EntityId victim, void* victim_move);

// The physical contact listeners of the fighters, and the replayed contact's state.
extern thread_local bool replaying_physical_contact;
extern std::unordered_map<void*, void*> physical_contact_listeners;
extern thread_local bool physical_contact_evaluated;
void* physical_contact_listener(void* movement);

}  // namespace kcds::game_client
