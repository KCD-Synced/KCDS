// The local player's combat input: relayed to the replicas with what the local game made of
// it (an attack, a refusal, a riposte), the held block of the test kit, the push availability, and
// the combat eligibility of the replicas.
#pragma once

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace kcds::game_client {

// The fighter's riposte window: the combat-model bool property at core + 2096 that the riposte
// trigger (C_CombatTriggerRiposte) sets while it is open (slot 14) and
// clears when it closes (slot 15). An attack press or release while it is open asks for the
// riposte (->): it sets the riposte request, the bool property at
// core + 912, and makes no attack. The perfect block goes on into the riposte while the request is
// set, and the game clears it when the riposte ends (set at the press, kept through the
// riposte's states 4, 8, 16 and 32).
constexpr std::size_t kCoreRiposteWindow = 2096;
constexpr std::size_t kCoreRiposteRequest = 912;
// A bool property's value; the game's setter also tells its listeners.
constexpr std::size_t kBoolPropertyValue = 8;

extern std::atomic<bool> native_block_held;
extern std::atomic<bool> native_prepare_held;
extern double native_block_test_until;
extern thread_local bool native_block_test_call;
// The game's bool-property setter (it also tells the property's listeners).
using SetBoolProperty = void(__fastcall*)(void* property, char value);
extern SetBoolProperty set_bool_property;
std::uint8_t* combat_core_of(void* actor);
bool riposte_window_open(void* actor);
bool riposte_requested(void* actor);
bool attack_release_held(void* actor);
// The game's own push (F) availability of a clinch component.
using ClinchCheck = bool(__fastcall*)(void* clinch);
extern ClinchCheck native_push_available;
int push_available(void* movement);
void __fastcall hooked_combat_input(void* self, const void* action, int mode, float value);
char __fastcall hooked_combat_eligibility(void* self, void* source, void* target);

}  // namespace kcds::game_client
