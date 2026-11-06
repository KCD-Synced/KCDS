// The game's attack factory and its attacks: the owner's pair selection applied to a
// replica's attack, the refused-pair bookkeeping, the local swing's start, the neutral attack's
// space query as a player's, and the attack start and exit observers.
#pragma once

#include <Windows.h>

#include <cstdint>
#include <unordered_map>

namespace kcds::game_client {

// The neutral attack's space query (hooked).
using NativeAttackSpace = std::intptr_t(__fastcall*)(void*, void*);
extern NativeAttackSpace original_attack_space;
std::intptr_t __fastcall hooked_attack_space(void* self, void* row);
// The attack factory (hooked; its original is original_make_attack, combat/sync_attacks.hpp).
void* __fastcall hooked_make_attack(void* factory, void** out, unsigned attack, unsigned flags);
// The attack's Start and Exit (observed), and when each native attack started.
using NativeAttackStart = char(__fastcall*)(void*, void*);
using NativeAttackExit = std::intptr_t(__fastcall*)(void*);
extern NativeAttackStart original_attack_start;
extern NativeAttackExit original_attack_exit;
extern std::unordered_map<void*, ULONGLONG> native_attack_started;
char __fastcall hooked_attack_start(void* action, void* context);
std::intptr_t __fastcall hooked_attack_exit(void* action);

}  // namespace kcds::game_client
