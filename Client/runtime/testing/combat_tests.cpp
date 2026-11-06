// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "combat_tests.hpp"

#include "../combat/combat_input.hpp"
#include "../combat/defence.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "test_hooks.hpp"

namespace kcds::game_client {

HANDLE native_attack_test_event = nullptr;
double native_attack_test_until = 0, native_attack_test_next = 0, native_attack_test_release = 0;
void tick_native_attack_test(void* actor, double now) {
  const bool requested =
      native_attack_test_event && WaitForSingleObject(native_attack_test_event, 0) == WAIT_OBJECT_0;
  if (requested) {
    native_attack_test_until = now + 30;
    native_attack_test_next = now;
    log("COMBAT TEST native attacks started actor=%p duration=30s", actor);
  }
  const bool release = native_attack_test_release > 0 && now >= native_attack_test_release;
  const bool expired = native_attack_test_until > 0 && now >= native_attack_test_until;
  if (!requested && !release && !expired &&
      !(native_attack_test_until > now && now >= native_attack_test_next))
    return;
  auto movement = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(actor) + 632);
  auto controller = movement ? *reinterpret_cast<std::uint8_t**>(movement + 1104) : nullptr;
  auto native_input = controller ? *reinterpret_cast<void**>(controller + 40) : nullptr;
  auto registry =
      *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(get_services()) + 48);
  if (!native_input || !registry)
    return;
  if (release || expired) {
    if (native_attack_test_release > 0)
      hooked_combat_input(native_input, registry + 272, 2, 0.f);
    native_attack_test_release = 0;
    if (expired) {
      native_attack_test_until = 0;
      log("COMBAT TEST native attacks stopped");
      return;
    }
  }
  if (native_attack_test_until > now && now >= native_attack_test_next &&
      !native_attack_test_release) {
    native_attack_test_next = now + 2.25;
    const auto kind = native_action_type(movement);
    auto core = *reinterpret_cast<std::uint8_t**>(movement + 528);
    if (core && *reinterpret_cast<void**>(core + 3320) && (kind == 0 || kind == 1)) {
      hooked_combat_input(native_input, registry + 272, 1, 1.f);
      native_attack_test_release = now + 0.12;
      log("COMBAT TEST native attack press; original controller determines attack");
    }
  }
}
void tick_native_block_test(void* actor, double now) {
  const bool requested =
      native_block_test_event && WaitForSingleObject(native_block_test_event, 0) == WAIT_OBJECT_0;
  const bool expired = native_block_test_until > 0 && now >= native_block_test_until;
  if (!requested && !expired)
    return;
  auto movement = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(actor) + 632);
  auto controller = movement ? *reinterpret_cast<std::uint8_t**>(movement + 1104) : nullptr;
  auto native_input = controller ? *reinterpret_cast<void**>(controller + 40) : nullptr;
  auto registry =
      *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(get_services()) + 48);
  if (!native_input || !registry)
    return;
  native_block_test_until = requested ? now + 30 : 0;
  native_block_test_call = true;
  hooked_combat_input(native_input, registry + 296, requested ? 1 : 2, requested ? 1.f : 0.f);
  native_block_test_call = false;
  log("COMBAT TEST native held block %s actor=%p auto-release=30s",
      requested ? "started" : "released", actor);
}
// Test hook (Local\KCD2S.MasterStrikeTest.<pid>): the local fighter starts the master strike the
// way the game's master-strike trigger does (kind 5, any attacker, no flag), so its
// relay can be checked with test characters whose skills never roll one.
HANDLE master_strike_test_event = nullptr;
void tick_master_strike_test(void* actor) {
  if (!master_strike_test_event || !start_defence || !perfect_block_request ||
      WaitForSingleObject(master_strike_test_event, 0) != WAIT_OBJECT_0) {
    return;
  }
  auto movement = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + 632);
  void* action = nullptr;
  start_defence(movement, &action, kMasterStrikeKind, 0, 0, *perfect_block_request);
  log("COMBAT TEST master strike made=%u action=%u", unsigned(action != nullptr),
      native_action_type(movement));
  if (action) {
    (*reinterpret_cast<void(__fastcall***)(void*)>(action))[2](action);
  }
}

}  // namespace kcds::game_client
