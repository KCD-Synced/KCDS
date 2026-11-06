// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "attack_observers.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../replicas/remote.hpp"
#include "../riding/rider_diagnostics.hpp"
#include "refused_pair.hpp"
#include "sync_attacks.hpp"

namespace kcds::game_client {

namespace integration = kcd2s::integration;

CombatEntry original_combat_entry = nullptr;
BodyCombatEntry native_body_combat_entry = nullptr;
NativeMovementCombatEntry original_movement_combat_entry = nullptr;
struct CombatEntryObservation {
  void* movement = nullptr;
  bool eligibility_observed = false;
  char eligibility_result = 0;
};
thread_local CombatEntryObservation* combat_entry_observation = nullptr;
NativeActorAnimationStart original_actor_animation_start = nullptr;

char __fastcall observed_actor_animation_start(void* action, void* context) {
  if (GetCurrentThreadId() != engine_update_thread.load() || !action) {
    // Native animation workers may enter this method too. Record only scalar
    // identity/callers there; engine object reads remain on the update thread.
    log("ACTOR ANIMATION entry unobserved action=%p thread=%lu engine_thread=%lu tick=%llu", action,
        GetCurrentThreadId(), engine_update_thread.load(), GetTickCount64());
    log_native_callers("ACTOR ANIMATION entry unobserved");
    return original_actor_animation_start(action, context);
  }
  auto bytes = static_cast<std::uint8_t*>(action);
  auto manager = *reinterpret_cast<std::uint8_t**>(bytes + 152);
  auto actor = manager ? *reinterpret_cast<std::uint8_t**>(manager + 40) : nullptr;
  auto movement = actor ? *reinterpret_cast<void**>(actor + 632) : nullptr;
  const bool local = actor && actor == local_combat_actor.load();
  const auto source = local ? 0 : remote_combat_id(movement);
  if (!local && !source) {
    log("ACTOR ANIMATION entry unknown action=%p actor=%p movement=%p tick=%llu", action, actor,
        movement, GetTickCount64());
    return original_actor_animation_start(action, context);
  }
  auto data = *reinterpret_cast<std::uint8_t**>(bytes + 80);
  const auto animation = data ? *reinterpret_cast<unsigned*>(data + 12) : 0;
  const char result = original_actor_animation_start(action, context);
  log("ACTOR ANIMATION start local=%u source=%u action=%p animation=%u accepted=%u tick=%llu",
      unsigned(local), source, action, animation, unsigned(result), GetTickCount64());
  log_native_callers("ACTOR ANIMATION start");
  return result;
}

// Test-kit probe of original movement entry. Observe the nested eligibility call
// in its existing hook; never repeat a native check or change its return value.
char __fastcall observed_movement_combat_entry(void* movement, const void* request) {
  if (GetCurrentThreadId() != engine_update_thread.load() || !movement || !request) {
    return original_movement_combat_entry(movement, request);
  }
  auto actor = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(movement) + 504);
  const bool local = actor && actor == local_combat_actor.load();
  const auto source = local ? 0 : remote_combat_id(movement);
  if (!local && !source) {
    return original_movement_combat_entry(movement, request);
  }
  auto core = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement) + 528);
  const unsigned before = core ? unsigned(core[8]) : 0;
  CombatEntryObservation observation{movement};
  auto previous = combat_entry_observation;
  combat_entry_observation = &observation;
  char result = 0;
  __try {
    result = original_movement_combat_entry(movement, request);
  } __finally {
    combat_entry_observation = previous;
  }
  log("COMBAT ENTRY local=%u source=%u accepted=%u before=%u after=%u eligibility_seen=%u "
      "eligibility=%u replay=%u matching_replay=%u tick=%llu",
      unsigned(local), source, unsigned(result), before, core ? unsigned(core[8]) : 0,
      unsigned(observation.eligibility_observed), unsigned(observation.eligibility_result),
      unsigned(replaying_combat_input), unsigned(movement == replayed_combat_movement),
      GetTickCount64());
  return result;
}

// Test-kit observations of the original perfect-block riposte predicate. Neither
// probe invokes an eligibility check a second time or changes its return value.
NativeCombatPredicate original_riposte_eligibility = nullptr;
NativeCombatPredicate original_movement_can_attack = nullptr;
NativeCombatPredicate original_riposte_opponent = nullptr;
struct RiposteEligibilityObservation {
  void* movement = nullptr;
  bool can_attack_observed = false;
  bool can_attack = false;
};
thread_local RiposteEligibilityObservation* riposte_eligibility_observation = nullptr;

// The original attack-request submission check, called by the attack press and
// release handlers right after the attack factory: it refuses an attack
// whose row the game marked for a pair (attack + 98 or + 99). Test kits (KCDS_TEST_HOOKS) also
// observe it and its nested player-controller check (728860), capturing action bytes before the
// original releases its temporary reference. Neither probe repeats a query.
NativeAttackRequestCheck original_attack_request_check = nullptr;
NativeAttackControllerCheck original_attack_controller_check = nullptr;
struct AttackRequestObservation {
  void* movement = nullptr;
  bool controller_observed = false;
  bool controller_blocked = false;
};
thread_local AttackRequestObservation* attack_request_observation = nullptr;
// Whether the test kit's probes log every attack request (KCDS_TEST_HOOKS).
bool attack_request_probe = false;
// The replica attack made right after its own pair (sync) attack choice was refused
// (hooked_choose_sync_attack, combat/refused_pair.hpp; engine thread). The game marked that
// ordinary attack for the pair it wanted (action + 98), and the attack request check then refused
// it: the replica did not swing at all where its owner's game made an ordinary attack
// (a fast exchange's third swing hit the defender with no swing on the attacker's replica). The
// owner's game decides: a pair it chose arrives as a SyncHit owner selection, an attack it refused
// as AttackRefused. The check is hooked in every build (it was a test-kit probe only, so the
// players' own games never cleared the mark).
kcds::runtime::combat::RefusedPair refused_pairs;
constexpr std::size_t kAttackWantsPair = 98;
constexpr std::size_t kAttackOutOfPair = 99;

char __fastcall observed_attack_controller_check(void* movement, void* target, char enabled) {
  const char result = original_attack_controller_check(movement, target, enabled);
  auto observation = attack_request_observation;
  if (observation && observation->movement == movement) {
    observation->controller_observed = true;
    observation->controller_blocked = result != 0;
  }
  return result;
}

char call_observed_attack_request(void* controller, void** action,
                                  AttackRequestObservation* observation) {
  auto previous = attack_request_observation;
  char result = 0;
  __try {
    attack_request_observation = observation;
    result = original_attack_request_check(controller, action);
  } __finally {
    attack_request_observation = previous;
  }
  return result;
}

char __fastcall hooked_attack_request_check(void* controller, void** action) {
  if (GetCurrentThreadId() != engine_update_thread.load() || !controller || !action || !*action) {
    return original_attack_request_check(controller, action);
  }
  auto requested = static_cast<std::uint8_t*>(*action);
  auto movement = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(controller) + 24);
  const auto source = remote_combat_id(movement);
  // Only the ordinary attack (kind 3) the replica's factory just made after its refused pair
  // choice; every other check drops that record (combat/refused_pair.hpp).
  const bool refused_pair = refused_pairs.take(reinterpret_cast<std::uintptr_t>(requested));
  unsigned refused_mark = 0;
  if (refused_pair && source && requested[kAttackWantsPair]) {
    refused_mark = requested[kAttackWantsPair];
    requested[kAttackWantsPair] = 0;
    log("COMBAT replica attack kept ordinary: its own pair choice was refused source=%u", source);
  }
  if (!attack_request_probe) {
    return original_attack_request_check(controller, action);
  }
  auto actor = movement ? *reinterpret_cast<void**>(movement + 504) : nullptr;
  const bool local = actor && actor == local_combat_actor.load();
  if (!local && !source) {
    return original_attack_request_check(controller, action);
  }
  const auto kind = slot<unsigned(__fastcall*)(void*)>(requested, 14)(requested);
  if (kind != kNativeAttackAction) {
    return original_attack_request_check(controller, action);
  }
  const unsigned flag98 = refused_mark ? refused_mark : requested[kAttackWantsPair];
  const unsigned flag99 = requested[kAttackOutOfPair];
  auto core = movement ? *reinterpret_cast<std::uint8_t**>(movement + 528) : nullptr;
  void* target = core ? *reinterpret_cast<void**>(core + 3320) : nullptr;
  AttackRequestObservation observation{movement};
  const char result = call_observed_attack_request(controller, action, &observation);
  log("ATTACK REQUEST local=%u source=%u action=%p kind=%u flag98=%u flag99=%u "
      "controller_observed=%u controller_blocked=%u accepted=%u target=%p tick=%llu",
      unsigned(local), source, requested, kind, flag98, flag99,
      unsigned(observation.controller_observed), unsigned(observation.controller_blocked),
      unsigned(result != 0), target, GetTickCount64());
  return result;
}

bool __fastcall observed_riposte_opponent(void* action) {
  const bool result = original_riposte_opponent(action);
  if (GetCurrentThreadId() != engine_update_thread.load() || !action) {
    return result;
  }
  auto native_action = static_cast<std::uint8_t*>(action);
  auto movement = *reinterpret_cast<std::uint8_t**>(native_action + 136);
  auto actor = movement ? *reinterpret_cast<void**>(movement + 504) : nullptr;
  const bool local = actor && actor == local_combat_actor.load();
  const auto source = remote_combat_id(movement);
  if (!local && !source) {
    return result;
  }
  auto core = movement ? *reinterpret_cast<std::uint8_t**>(movement + 528) : nullptr;
  auto target = core ? *reinterpret_cast<std::uint8_t**>(core + 3320) : nullptr;
  auto target_core = target ? *reinterpret_cast<std::uint8_t**>(target + 528) : nullptr;
  const auto kind = slot<unsigned(__fastcall*)(void*)>(action, 14)(action);
  const bool master = kind == 9;
  log("COMBAT riposte opponent source=%u local=%u action=%p allowed=%u target=%u "
      "target_state=%u target_flag=%u kind=%u checked=%u phase=%u finished=%u tick=%llu",
      source, unsigned(local), action, unsigned(result), remote_combat_id(target),
      target_core ? *reinterpret_cast<unsigned*>(target_core + 56) : 0,
      target_core ? unsigned(target_core[1160]) : 0, kind,
      unsigned(native_action[master ? 177 : 185]),
      *reinterpret_cast<unsigned*>(native_action + (master ? 216 : 224)),
      unsigned(native_action[master ? 176 : 184]), GetTickCount64());
  return result;
}

bool __fastcall observed_movement_can_attack(void* movement) {
  const bool result = original_movement_can_attack(movement);
  auto observation = riposte_eligibility_observation;
  if (observation && observation->movement == movement) {
    observation->can_attack_observed = true;
    observation->can_attack = result;
  }
  return result;
}

bool call_observed_riposte_eligibility(void* action, RiposteEligibilityObservation* observation) {
  auto previous = riposte_eligibility_observation;
  bool result = false;
  __try {
    riposte_eligibility_observation = observation;
    result = original_riposte_eligibility(action);
  } __finally {
    riposte_eligibility_observation = previous;
  }
  return result;
}

bool __fastcall observed_riposte_eligibility(void* action) {
  if (GetCurrentThreadId() != engine_update_thread.load() || !action) {
    return original_riposte_eligibility(action);
  }
  auto movement = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(action) + 136);
  auto actor = movement ? *reinterpret_cast<void**>(movement + 504) : nullptr;
  const bool local = actor && actor == local_combat_actor.load();
  const auto source = remote_combat_id(movement);
  if (!local && !source) {
    return original_riposte_eligibility(action);
  }
  RiposteEligibilityObservation observation{movement};
  const bool result = call_observed_riposte_eligibility(action, &observation);
  auto native_core = movement ? *reinterpret_cast<std::uint8_t**>(movement + 528) : nullptr;
  auto target = native_core ? *reinterpret_cast<std::uint8_t**>(native_core + 3320) : nullptr;
  auto target_core = target ? *reinterpret_cast<std::uint8_t**>(target + 528) : nullptr;
  log("COMBAT riposte eligibility source=%u local=%u allowed=%u can_attack_seen=%u can_attack=%u "
      "target_state=%u target_flag=%u phase=%u requested=%u contact=%u",
      source, unsigned(local), unsigned(result), unsigned(observation.can_attack_observed),
      unsigned(observation.can_attack),
      target_core ? *reinterpret_cast<unsigned*>(target_core + 56) : 0,
      target_core ? unsigned(target_core[1160]) : 0,
      *reinterpret_cast<unsigned*>(static_cast<std::uint8_t*>(action) + 224),
      unsigned(static_cast<std::uint8_t*>(action)[232]),
      unsigned(static_cast<std::uint8_t*>(action)[233]));
  return result;
}

char __fastcall hooked_combat_entry(void* movement) {
  auto result = original_combat_entry(movement);
  if (combat_entry_observation && combat_entry_observation->movement == movement) {
    combat_entry_observation->eligibility_observed = true;
    combat_entry_observation->eligibility_result = result;
  }
  if (result || !replaying_combat_input || movement != replayed_combat_movement)
    return result;
  const auto source = remote_combat_id(movement);
  if (!source)
    return result;
  auto actor = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement) + 504);
  if (!actor)
    return result;
  // C_Player additionally checks its locally controlled
  // ActorStateExpansion. Remote C_Player has no local input authority.
  // Use the same original body/animation eligibility check, solely while
  // replaying this owner's actual input. Do not invent attacks or damage.
  const auto allowed = native_body_combat_entry(actor + 1952, 0);
  log("COMBAT replica neutral entry source=%u native=%u body_allowed=%u", source, unsigned(result),
      unsigned(allowed));
  return allowed ? 1 : 0;
}

}  // namespace kcds::game_client
