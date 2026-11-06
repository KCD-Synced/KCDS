// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "combat_input.hpp"

#include "../book/resource_book.hpp"
#include "../core/log.hpp"
#include "../input/resource_input.hpp"
#include "../core/natives.hpp"
#include "../npcs/npc_attack.hpp"
#include "../npcs/npc_health.hpp"
#include "../replicas/remote.hpp"
#include "combat_identity.hpp"
#include "defence.hpp"
#include "targetable.hpp"

#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>

namespace kcds::game_client {

namespace integration = kcd2s::integration;

std::atomic<bool> native_block_held{false}, native_prepare_held{false};
// When the local player last asked to lock on (GetTickCount64 ms; 0 for never): a replica of a
// server NPC near it is then engaged (npcs/native_npcs, NPC_ENGAGE).
std::atomic<ULONGLONG> npc_lock_asked_ms{0};
double native_block_test_until = 0;
thread_local bool native_block_test_call = false;
SetBoolProperty set_bool_property = nullptr;
std::uint8_t* combat_core_of(void* actor) {
  auto movement =
      actor ? *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(actor) + 632) : nullptr;
  return movement ? *reinterpret_cast<std::uint8_t**>(movement + 528) : nullptr;
}
bool riposte_window_open(void* actor) {
  auto core = combat_core_of(actor);
  return core && core[kCoreRiposteWindow + kBoolPropertyValue] != 0;
}
bool riposte_requested(void* actor) {
  auto core = combat_core_of(actor);
  return core && core[kCoreRiposteRequest + kBoolPropertyValue] != 0;
}
// The fighter's attack controller (movement + 1104) holds a release for later at + 248: its
// release handler sets it instead of making the attack while the press's attack is
// untargeted (core + 2904 state 5, a press without a lock), and its update makes
// the attack from it once the attack's minimum hold has run out.
constexpr std::size_t kMovementAttackController = 1104;
constexpr std::size_t kControllerReleaseHeld = 248;
bool attack_release_held(void* actor) {
  auto movement =
      actor ? *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(actor) + 632) : nullptr;
  auto controller =
      movement ? *reinterpret_cast<std::uint8_t**>(movement + kMovementAttackController) : nullptr;
  return controller && controller[kControllerReleaseHeld] != 0;
}
// A fighter's clinch component (C_CombatActorClinch, movement + 1008) and the game's own check
// whether its push (F, attack_special) is available now (its clinch state is a
// clinch state, it is in its guard state (2), and + 28 is set or the push
// timer at + 32 has run out; the clinch start sets that timer from
// wh_cs_SyncGuardBackoffTimer, registered with 2.0 s). The F press handler takes the
// press in a locked fight only then. Read only: logged with a push press; -1 when unknown.
// The clinch component is at kMovementClinch (core/client_internal.hpp).
ClinchCheck native_push_available = nullptr;
int push_available(void* movement) {
  if (!native_push_available || !movement) {
    return -1;
  }
  auto clinch = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(movement) + kMovementClinch);
  if (!clinch) {
    return -1;
  }
  return native_push_available(clinch) ? 1 : 0;
}
void __fastcall hooked_combat_input(void* self, const void* action, int mode, float value) {
  auto actor = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(self) + 24);
  const bool local = actor && actor == local_combat_actor.load();
  const auto name = action ? *static_cast<const char* const*>(action) : nullptr;
  const bool block = local && name && std::strcmp(name, "block") == 0;
  const bool prepare = local && name && std::strcmp(name, "prepare_to_attack") == 0;
  const bool intent =
      local && name &&
      (block || prepare || std::strcmp(name, "opponent_lock") == 0 ||
       std::strcmp(name, "attack_primary_mouse") == 0 || std::strcmp(name, "attack_primary") == 0 ||
       std::strcmp(name, "attack_special") == 0);
  // Development-only native API test: retain a held block while the user
  // switches windows. This does not change defense outcomes or RPG stats.
  if (block && mode == 2 && native_block_test_until > 0 && !native_block_test_call)
    return;
  if (block && (mode == 1 || mode == 2))
    native_block_held.store(mode == 1);
  if (prepare && (mode == 1 || mode == 2))
    native_prepare_held.store(mode == 1);
  if (local && name && (mode == 1 || mode == 2))
    log("COMBAT INPUT action=%.64s mode=%d value=%.3f", name, mode, value);
  // A game action a resource holds off does nothing here either (Controls.blockAction):
  // the seat's exits (sitting_exit_up and the others, the "sitting" action map) act on their
  // release, which reached the game this way, so a held-off W still got the player up from a
  // chess bench. The release of a press held off is held off too (hold_off); a press
  // seen here and in the player input both is held off or let through the same way twice.
  if (local && name && kcds::runtime::input::hold_off(name, unsigned(mode))) {
    return;
  }
  // A lock on: an NPC replica near the player is engaged (NPC_ENGAGE, npcs/native_npcs).
  if (local && name && mode == 1 && std::strcmp(name, "opponent_lock") == 0)
    npc_lock_asked_ms.store(GetTickCount64());
  if (local && name && mode == 1 && std::strcmp(name, "attack_special") == 0) {
    auto movement = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + 632);
    log("COMBAT INPUT push available=%d", push_available(movement));
  }
  if (local && name && mode == 1 &&
      (std::strcmp(name, "read_next") == 0 || std::strcmp(name, "read_prev") == 0)) {
    kcds::runtime::book::page_turned(std::strcmp(name, "read_next") == 0);
  }
  if (intent)
    ++manual_combat_scope;
  input_made_sync_perfect_block = false;
  local_attack_made = false;
  // Whether this input asks the game for the fighter's riposte: the game sets the request inside
  // the input (-> ->) and makes no attack of it.
  const bool riposte_before = local && riposte_requested(actor);
  original_combat_input(self, action, mode, value);
  const bool riposte_asked = local && !riposte_before && riposte_requested(actor);
  if (intent)
    --manual_combat_scope;
  if (local && !replaying_combat_input) {
    relay_native_target(actor);
    integration::CombatActionData data{action_kind(name), std::uint8_t(mode), value};
    const bool attack = data.action == integration::CombatAction::AttackMouse ||
                        data.action == integration::CombatAction::Attack;
    // The game repeats the press while the button is held (again 78 ms later): those are the
    // same press and are withheld until its release too.
    if (attack && mode == 1 && (input_made_sync_perfect_block || attack_press_withheld)) {
      if (!attack_press_withheld) {
        log("COMBAT INPUT attack press made the master strike: not relayed as an attack");
      }
      attack_press_withheld = true;
      return;
    }
    if (attack && mode == 2 && attack_press_withheld) {
      attack_press_withheld = false;
      return;
    }
    // An attack pressed during the fighter's own hit reaction never runs: the factory may queue it,
    // the game drops it when the reaction ends (four presses 0.6-1.2 s after the hit, none
    // swung). A replica shows the hit without the native hit action and swung.
    const bool in_hit_reaction =
        native_action_type(*reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + 632)) ==
        kNativeHitAction;
    // A release without a lock makes its attack in a later update (234 ms after the
    // release); the replica's game holds it the same way.
    const bool release_held = mode == 2 && attack_release_held(actor);
    if (attack && (mode == 1 || mode == 2) && riposte_asked) {
      // The replicas ask for the same riposte, whatever their own riposte window shows.
      log("COMBAT INPUT attack %s asked for the riposte", mode == 1 ? "press" : "release");
      relay_combat_action({integration::CombatAction::Riposte, 1, 0});
    } else if (attack && (mode == 1 || mode == 2) &&
               ((!local_attack_made && !release_held) || in_hit_reaction)) {
      // The game made no attack of this input here (a press during the recovery, the own parry
      // or a hit, live): the replicas must not make one of it either.
      relay_combat_action({integration::CombatAction::AttackRefused, 1, 0});
    }
    if (attack && (mode == 1 || mode == 2)) {
      // The attack factory picks the attack from the zone at core+384, read
      // through slot 1; send it first so the replica starts the same attack.
      auto movement = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(actor) + 632);
      auto core = movement ? *reinterpret_cast<std::uint8_t**>(movement + 528) : nullptr;
      if (core) {
        const auto zone = slot<unsigned(__fastcall*)(void*)>(core + 384, 1)(core + 384);
        integration::CombatActionData zone_data{integration::CombatAction::AttackZone, 1,
                                                float(zone)};
        if (integration::valid_combat_action(zone_data))
          relay_combat_action(zone_data);
      }
    }
    if ((mode == 1 || mode == 2 || mode == 4) && integration::valid_combat_action(data))
      relay_combat_action(data);
  }
}
char __fastcall hooked_combat_eligibility(void* self, void* source, void* target) {
  const auto native = original_combat_eligibility(self, source, target);
  if (native)
    return native;
  // An NPC the server told to attack, and its target, are enemies (Npc.attack).
  if (kcds::runtime::npcs::attack::enemies(source, target)) {
    static ULONGLONG logged_at = 0;
    if (GetTickCount64() - logged_at > 2000) {
      logged_at = GetTickCount64();
      log("NPC attack enemy check source=%p target=%p: enemies", source, target);
    }
    return 1;
  }
  auto actor = static_cast<std::uint8_t*>(local_combat_actor.load());
  if (!actor || !target)
    return native;
  if (source != *reinterpret_cast<void**>(actor + 1640)) {
    std::lock_guard<std::mutex> lock(combat_identity_mutex);
    auto remote = remote_native_targets.find(source);
    if (remote == remote_native_targets.end())
      return native;
    const bool selected = combat_soul_of(remote->second) == target;
    // A watcher's two replicas fight each other as their owners do.
    if (selected && remote_combat_souls.count(target))
      return 1;
    if (target != *reinterpret_cast<void**>(actor + 1640))
      return native;
    auto movement = remote_combat_movements.find(source);
    // Native attack actions and an original received combat input are intent.
    // A neutral replica does not become hostile just by being nearby.
    const bool attacking =
        source == remote_manual_combat_soul ||
        (replicated_attack_actor &&
         source == *reinterpret_cast<void**>(static_cast<std::uint8_t*>(replicated_attack_actor) +
                                             1640)) ||
        (movement != remote_combat_movements.end() && native_attack_active(movement->second));
    return selected || attacking ? 1 : native;
  }
  // The network id of a remote player's body, for Player.setLockable.
  core::EntityId remote_id = 0;
  {
    std::lock_guard<std::mutex> lock(combat_identity_mutex);
    const auto remote = remote_combat_souls.find(target);
    const bool remote_player = remote != remote_combat_souls.end();
    if (remote_player) {
      remote_id = remote->second;
    }
    // A server NPC is no enemy of the game's until it fights (a replica runs no AI at all): the
    // player locks on to any body of one on the same terms as on another player's replica, so
    // a neutral NPC is a target as soon as the player asks.
    std::optional<kcds::runtime::npcs::health::Body> npc_body;
    if (!remote_player) {
      npc_body = kcds::runtime::npcs::health::body_of_soul(target);
    }
    if (!remote_player && !npc_body)
      return native;
  }
  // Only an original combat input or an already selected native target may
  // extend enemy eligibility. Never set global hostility/LockOnAllTargets.
  auto movement = *reinterpret_cast<std::uint8_t**>(actor + 632);
  // A player a script took out of the lock-on is an enemy only for a swing in progress: the game's
  // picker passes over it and lets go of it, while a blow that lands still lands.
  if (remote_id && kcds::runtime::combat::targetable::untargetable(std::uint32_t(remote_id))) {
    return native_attack_active(movement) ? 1 : native;
  }
  auto core = movement ? *reinterpret_cast<std::uint8_t**>(movement + 528) : nullptr;
  auto selected = core ? *reinterpret_cast<std::uint8_t**>(core + 3320) : nullptr;
  auto selected_actor = selected ? *reinterpret_cast<std::uint8_t**>(selected + 504) : nullptr;
  const bool retained =
      selected_actor && *reinterpret_cast<void**>(selected_actor + 1640) == target;
  // The actual hit occurs after the input scope has ended. A native Attack
  // action in any priority slot remains intent without a selected/locked target.
  const bool requested = manual_combat_scope || native_block_held.load() ||
                         native_prepare_held.load() || native_attack_active(movement);
  bool previously_requested = false;
  auto controller = movement ? *reinterpret_cast<std::uint8_t**>(movement + 1104) : nullptr;
  if (controller && controller[192]) {
    std::lock_guard<std::mutex> lock(combat_identity_mutex);
    auto it = remote_combat_souls.find(target);
    previously_requested =
        it != remote_combat_souls.end() && it->second == local_requested_target.load();
  }
  // A hit may temporarily clear the native selected pointer. A still-active
  // user hard-lock keeps this same neutral player eligible through that transition.
  // The native controller still decides distance, line of sight, and unlocking.
  if (!requested && !retained && !previously_requested)
    return native;
  static double next = 0;
  const auto now = local::clock_seconds();
  if (now >= next) {
    next = now + 1;
    log("COMBAT manual eligibility source=%p target=%p requested=%u retained=%u", source, target,
        unsigned(requested), unsigned(retained));
  }
  return 1;
}

}  // namespace kcds::game_client
