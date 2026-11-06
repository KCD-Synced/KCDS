// The game's actor-owned interactive state and its global action maps: a replica's get-up, state
// handler or movement restriction keeps its own state and never disables the local player's keys.
// action_map_manager is declared in core/client_internal.hpp.
#pragma once

#include <cstdint>

namespace kcds::game_client {

// The actor whose native interactive state is being applied now (engine thread).
extern thread_local void* input_mode_actor;
using InteractiveState = std::intptr_t(__fastcall*)(void*, void*, void*, const void*);
using NativeMoveToStart = double(__fastcall*)(void*);
using NativeMoveToEnd = void(__fastcall*)(void*, unsigned, char);
using EnableActionMap = void(__fastcall*)(void*, const char*, bool);
extern InteractiveState original_interactive_state;
extern NativeMoveToStart original_move_to_start;
extern NativeMoveToEnd original_move_to_end;
extern EnableActionMap original_enable_action_map;
std::intptr_t __fastcall hooked_interactive_state(void* self, void* out, void* actor,
                                                  const void* event);
double __fastcall hooked_move_to_start(void* self);
void __fastcall hooked_move_to_end(void* self, unsigned request, char result);
void __fastcall hooked_enable_action_map(void* self, const char* name, bool enabled);
// C_PlayerStateHandler's non-interactive push and pop.
using PlayerStateNoninteractive = void(__fastcall*)(void*);
extern PlayerStateNoninteractive original_player_state_push;
extern PlayerStateNoninteractive original_player_state_pop;
void __fastcall hooked_player_state_push(void* handler);
void __fastcall hooked_player_state_pop(void* handler);
// The model context's movement restriction setter (observed only).
using SetMovementRestriction = std::intptr_t(__fastcall*)(void*, unsigned char, char);
extern SetMovementRestriction original_set_movement_restriction;
std::intptr_t __fastcall hooked_set_movement_restriction(void* self, unsigned char scope,
                                                         char enabled);

}  // namespace kcds::game_client
