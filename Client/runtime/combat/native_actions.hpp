// What a fighter's native combat does now: its primary action and active attack, the
// paired actions and the clinch guard that align a pair, the observer's preview of a relayed
// contact, the channel a replica is given in its own attack, the control-state log, and the
// SyncAttack exit and update observers. native_action_type, native_attack_active, paired_action,
// native_pair_alignment and log_native_control_state are declared in core/client_internal.hpp.
#pragma once

#include <Windows.h>

#include <array>
#include <cstdint>

#include "kcd2s/integration/combat.hpp"

namespace kcds::game_client {

// Logs the local and a replica's feedback component around a replica's spawn and removal.
void log_local_feedback(const char* event, void* remote_actor);
void apply_observer_contact(const kcd2s::integration::MeleeEvent& impact, void* source_actor,
                            unsigned source_engine, void* target_actor, unsigned target_engine);
bool native_neutral_attack_active(void* movement);
bool has_guard_sync_movement(void* movement);
bool native_clinch_owns_guard(void* movement);
bool owner_guard_for_held_clinch(void* movement, const std::array<unsigned, 3>& owner_model);

// The actor channel check (hooked: a replica is a player in its own attack).
using ActorHasChannel = bool(__fastcall*)(void*);
extern ActorHasChannel original_actor_has_channel;
bool __fastcall hooked_actor_has_channel(void* actor);

// The SyncAttack exit and update (observed).
extern ULONGLONG last_local_pair_exit;
using SyncAttackExit = std::intptr_t(__fastcall*)(void*);
extern SyncAttackExit original_sync_attack_exit;
std::intptr_t __fastcall hooked_sync_attack_exit(void* self);
using SyncAttackUpdate = unsigned(__fastcall*)(void*, float);
extern SyncAttackUpdate original_sync_attack_update;
unsigned __fastcall hooked_sync_attack_update(void* self, float delta);

}  // namespace kcds::game_client
