#include "hit_feedback.hpp"

#include "../core/client_internal.hpp"
#include "../core/log.hpp"

namespace kcds::game_client {

PlayerHitFeedback original_player_hit_feedback;
ActorHitFeedback base_actor_hit_feedback = nullptr;
std::intptr_t __fastcall hooked_player_hit_feedback(void* actor, const void* info) {
  // (C_Player v225) unconditionally calls through the player feedback component at
  // C_Player+3272. Only the local player owns that component; a remote C_Player replica keeps the
  // constructor's null. A hit event routed to a replica would dereference null and
  // terminate the game. Its base actor hit handling still cancels native actions, so preserve
  // that call and omit only the absent local feedback component.
  auto feedback = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + 3272);
  if (!feedback) {
    base_actor_hit_feedback(actor, info);
    log("COMBAT PLAYER base hit feedback applied: actor=%p has no local feedback component "
        "local=%p",
        actor, local_combat_actor.load());
    return 0;
  }
  auto soul = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(actor) + 1640);
  const float before = soul ? *reinterpret_cast<float*>(soul + 1800) : 0;
  auto result = original_player_hit_feedback(actor, info);
  if (actor == local_combat_actor.load())
    log("COMBAT PLAYER native hit feedback health_cache=%.5f->%.5f", before,
        soul ? *reinterpret_cast<float*>(soul + 1800) : 0);
  return result;
}

}  // namespace kcds::game_client
