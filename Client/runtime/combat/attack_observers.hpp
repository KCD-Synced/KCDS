// The game's own combat entry and attack request checks: the attack request check that clears a
// refused pair's mark on a replica's attack, and the test kit's observations of the
// combat entry, the riposte eligibility and the actor animation start.
#pragma once

#include "refused_pair.hpp"

namespace kcds::game_client {

// Combat entry (hooked), the body's combat entry check, and the movement's combat entry.
using CombatEntry = char(__fastcall*)(void*);
using BodyCombatEntry = bool(__fastcall*)(void*, unsigned);
extern CombatEntry original_combat_entry;
extern BodyCombatEntry native_body_combat_entry;
using NativeMovementCombatEntry = char(__fastcall*)(void*, const void*);
extern NativeMovementCombatEntry original_movement_combat_entry;
using NativeActorAnimationStart = char(__fastcall*)(void*, void*);
extern NativeActorAnimationStart original_actor_animation_start;
char __fastcall observed_actor_animation_start(void* action, void* context);
char __fastcall observed_movement_combat_entry(void* movement, const void* request);
char __fastcall hooked_combat_entry(void* movement);

// The riposte eligibility and its nested checks (observed by the test kit).
using NativeCombatPredicate = bool(__fastcall*)(void*);
extern NativeCombatPredicate original_riposte_eligibility;
extern NativeCombatPredicate original_movement_can_attack;
extern NativeCombatPredicate original_riposte_opponent;
bool __fastcall observed_riposte_opponent(void* action);
bool __fastcall observed_movement_can_attack(void* movement);
bool __fastcall observed_riposte_eligibility(void* action);

// The attack request submission check and its player-controller check.
using NativeAttackRequestCheck = char(__fastcall*)(void*, void**);
using NativeAttackControllerCheck = char(__fastcall*)(void*, void*, char);
extern NativeAttackRequestCheck original_attack_request_check;
extern NativeAttackControllerCheck original_attack_controller_check;
// Whether the test kit's probes log every attack request (KCDS_TEST_HOOKS).
extern bool attack_request_probe;
extern kcds::runtime::combat::RefusedPair refused_pairs;
char __fastcall observed_attack_controller_check(void* movement, void* target, char enabled);
char __fastcall hooked_attack_request_check(void* controller, void** action);

}  // namespace kcds::game_client
