// The player's hit feedback (C_Player v225), kept from a replica's absent feedback component.
#pragma once

#include <cstdint>

namespace kcds::game_client {

using PlayerHitFeedback = std::intptr_t(__fastcall*)(void*, const void*);
extern PlayerHitFeedback original_player_hit_feedback;
using ActorHitFeedback = void(__fastcall*)(void*, const void*);
extern ActorHitFeedback base_actor_hit_feedback;
std::intptr_t __fastcall hooked_player_hit_feedback(void* actor, const void* info);

}  // namespace kcds::game_client
