// The combat HUD and its denied-action presentation kept to the local player: the HUD cursor's
// combat input, the denied-action feedback and its UI event, ShowDenialAttack and the F key switch.
#pragma once

#include <cstdint>

namespace kcds::game_client {

using HudCombatInput = void*(__fastcall*)();
extern HudCombatInput original_hud_combat_input;
void* __fastcall hooked_hud_combat_input();
using DeniedActionFeedback = std::intptr_t(__fastcall*)(void*, const void*);
extern DeniedActionFeedback original_denied_action_feedback;
std::intptr_t __fastcall hooked_denied_action_feedback(void* self, const void* effect);
using DeniedUiEmit = char(__fastcall*)(void*, char);
extern DeniedUiEmit original_denied_ui_emit;
char __fastcall hooked_denied_ui_emit(void* self, char kind);
using ShowDenialAttack = std::intptr_t(__fastcall*)(void*);
extern ShowDenialAttack original_show_denial_attack;
std::intptr_t __fastcall hooked_show_denial_attack(void* controller);
using CombatUiPushKey = void(__fastcall*)(void* ui);
extern CombatUiPushKey original_combat_ui_push_key;
void __fastcall hooked_combat_ui_push_key(void* ui);

}  // namespace kcds::game_client
