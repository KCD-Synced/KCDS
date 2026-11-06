// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "attack_factory.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../replicas/remote.hpp"
#include "attack_observers.hpp"
#include "combat_identity.hpp"
#include "melee_contacts.hpp"
#include "native_actions.hpp"
#include "sync_attacks.hpp"

#include <mutex>
#include <unordered_map>

namespace kcds::game_client {

namespace integration = kcd2s::integration;

// MCP sets Attack+153 when original space query686848
// fails. Query14 (905EE4) and its cache906988 select player vs channel-0
// behavior through ActorHasChannel. Scope only this original neutral query;
// do not change actor channels, bypass collision, or touch local HUD ownership.
NativeAttackSpace original_attack_space = nullptr;
std::intptr_t __fastcall hooked_attack_space(void* self, void* row) {
  auto movement =
      self ? *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(self) + 8) : nullptr;
  const auto source = remote_combat_id(movement);
  const bool neutral = source && native_neutral_attack_active(movement);
  auto previous = replicated_attack_actor;
  if (neutral)
    replicated_attack_actor = *reinterpret_cast<void**>(movement + 504);
  const auto result = original_attack_space(self, row);
  replicated_attack_actor = previous;
  static ULONGLONG next_log = 0;
  if (neutral && GetTickCount64() >= next_log) {
    next_log = GetTickCount64() + 500;
    log("ATTACK native space source=%u result=%u player_query=1 tick=%llu", source,
        unsigned(result & 255), GetTickCount64());
  }
  return result;
}
void* __fastcall hooked_make_attack(void* factory, void** out, unsigned attack, unsigned flags) {
  auto move = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(factory) + 8);
  auto id = remote_combat_id(move);
  // The refused pair record is the engine thread's (inputs and their request checks run there).
  const bool engine_thread = GetCurrentThreadId() == engine_update_thread.load();
  const auto factory_movement = reinterpret_cast<std::uintptr_t>(move);
  if (engine_thread) {
    refused_pairs.factory_started(factory_movement);
  }
  // A left-out press replayed just before its release (dropped_attack_presses) made nothing in its
  // owner's game, so the owner's pair selection waiting now came with the release that follows:
  // the press does not take it (the press handler would only check and drop it,
  // and the release would then make an ordinary attack instead of the pair).
  if (id && replaying_combat_input && move == replayed_combat_movement &&
      !replaying_refused_press) {
    if (auto it = pending_sync_hits.find(id); it != pending_sync_hits.end()) {
      auto data = it->second.data;
      auto received = it->second.received;
      pending_sync_hits.erase(it);
      auto core = move ? *reinterpret_cast<std::uint8_t**>(move + 528) : nullptr;
      auto target = core ? *reinterpret_cast<std::uint8_t**>(core + 3320) : nullptr;
      auto actor = target ? *reinterpret_cast<void**>(target + 504) : nullptr;
      auto attack_row = find_native_sync_attack(data);
      auto hit_row = find_native_sync_hit(data);
      // The pair's target here: the local player, or on a watcher the replica of the player the
      // attacker's lock is on (TargetOther).
      const bool target_here =
          actor && (actor == local_combat_actor.load() || remote_combat_id(target));
      if (GetTickCount64() - received <= 2000 && target_here && attack_row && hit_row) {
        auto result = construct_sync_attack(out, move);
        if (out && *out) {
          auto action = static_cast<std::uint8_t*>(*out);
          *reinterpret_cast<void**>(action + 88) = attack_row;
          *reinterpret_cast<void**>(action + 96) = hit_row;
          std::memcpy(action + 124, data.data() + 117, 4);
          log("COMBAT native pair selection applied source=%u attack=%p hit=%p action=%p", id,
              attack_row, hit_row, action);
        }
        return result;
      }
      log("COMBAT native pair selection rejected source=%u age=%llu target=%p attack=%p hit=%p", id,
          GetTickCount64() - received, actor, attack_row, hit_row);
    }
  }
  auto attack_actor =
      move ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(move) + 504) : nullptr;
  auto previous_attack_actor = replicated_attack_actor;
  if (id)
    replicated_attack_actor = attack_actor;
  auto result = original_make_attack(factory, out, attack, flags);
  replicated_attack_actor = previous_attack_actor;
  if (engine_thread) {
    std::uintptr_t ordinary_attack = 0;
    if (id && out && *out &&
        slot<unsigned(__fastcall*)(void*)>(*out, 14)(*out) == kNativeAttackAction) {
      ordinary_attack = reinterpret_cast<std::uintptr_t>(*out);
    }
    refused_pairs.factory_made(factory_movement, ordinary_attack);
  }
  auto actor = move ? *reinterpret_cast<void**>(move + 504) : nullptr;
  if (id && replaying_combat_input && move == replayed_combat_movement && out && *out) {
    const auto tracked = replica_contacts.find(id);
    const auto scope = replica_contact_scope(id, actor, move);
    if (tracked != replica_contacts.end() && scope) {
      const auto kind = slot<unsigned(__fastcall*)(void*)>(*out, 14)(*out);
      tracked->second.construct(*scope, reinterpret_cast<std::uintptr_t>(*out), kind);
    }
  }
  if (actor == local_combat_actor.load() && out && *out) {
    local_attack_made = true;
    const ULONGLONG now_ms = GetTickCount64();
    const bool same_swing =
        *out == local_attack_action && now_ms - local_attack_started < kSameSwingMs;
    if (!same_swing) {
      begin_local_combat_contact(*out);
    }
  }
  // The factory runs again for the release of the same swing; the swing started at the first.
  if (id && out && *out &&
      (id != remote_attack_source || GetTickCount64() - remote_attack_started >= kSameSwingMs)) {
    remote_attack_source = id;
    remote_attack_started = GetTickCount64();
    // The replica's own attack plays the game's attack animation, as on its owner's screen: its
    // strike comes from that animation. With the owner's clip in its place the game refused its
    // own, and its strike came 220-250 ms after the owner's, too late for a parry here.
    if (replaying_combat_input) {
      open_reaction_window(id);
    }
  }
  if (id || actor == local_combat_actor.load())
    log("COMBAT native Attack factory source=%u local=%u attack=%u flags=%u result=%p action=%p "
        "replay=%u",
        id, unsigned(actor == local_combat_actor.load()), attack, flags, result,
        out ? *out : nullptr, unsigned(replaying_combat_input));
  return result;
}
// MCP Attack primary183A61C58: Start v57 B27308, Exit v58 D4CAC0.
// Diagnostic hooks always invoke the original once and preserve its result.
NativeAttackStart original_attack_start = nullptr;
NativeAttackExit original_attack_exit = nullptr;
std::unordered_map<void*, ULONGLONG> native_attack_started;
void log_native_attack(const char* stage, void* action, int result, ULONGLONG elapsed) {
  auto a = static_cast<std::uint8_t*>(action);
  auto movement = *reinterpret_cast<void**>(a + 112);
  auto actor = movement
                   ? *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement) + 504)
                   : nullptr;
  const auto source = remote_combat_id(movement);
  const bool local = actor && actor == local_combat_actor.load();
  if (!local && !source)
    return;
  auto row = *reinterpret_cast<std::uint8_t**>(a + 88);
  auto definition = row ? *reinterpret_cast<std::uint8_t**>(row + 320) : nullptr;
  const auto record = definition ? *reinterpret_cast<std::uint32_t*>(definition + 8) : 0;
  auto core = movement
                  ? *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement) + 528)
                  : nullptr;
  auto target = core ? *reinterpret_cast<void**>(core + 3320) : nullptr;
  log("ATTACK LIFE %s local=%u source=%u action=%p kind=%u record=%u target=%p active=%u "
      "flags=0x%x native149=%u native150=%u done153=%u result=%d elapsed=%llu tick=%llu",
      stage, unsigned(local), source, action,
      slot<unsigned(__fastcall*)(void*)>(action, 14)(action), record, target, unsigned(a[40]),
      *reinterpret_cast<unsigned*>(a + 124), unsigned(a[149]), unsigned(a[150]), unsigned(a[153]),
      result, elapsed, GetTickCount64());
}
char __fastcall hooked_attack_start(void* action, void* context) {
  // A replica's attack is a player's attack. The Start gives an attack by an actor with a
  // channel (ActorHasChannel) the player's head start into its wind-up
  // (wh_cs_PlayerNormalAttackOffset 0.5 x the attack row's +344). Without it a
  // channel-0 replica wound up like an NPC: its strike came 0.29 s after its owner's on every other
  // screen (phase countdowns 0.552 against 0.268 s), so the owner's contact met a parry while
  // the replica still wound up, the parry turned the defender into the attacker and the native
  // counter (it needs the attacker striking) never started.
  auto movement = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(action) + 112);
  auto previous_attack_actor = replicated_attack_actor;
  if (movement && remote_combat_id(movement)) {
    replicated_attack_actor = *reinterpret_cast<void**>(movement + 504);
  }
  const auto result = original_attack_start(action, context);
  replicated_attack_actor = previous_attack_actor;
  native_attack_started[action] = GetTickCount64();
  if (result && movement &&
      *reinterpret_cast<void**>(movement + 504) == local_combat_actor.load()) {
    local_swing_start(action);
  }
  const auto source = remote_combat_id(movement);
  const auto tracked = replica_contacts.find(source);
  if (result && tracked != replica_contacts.end()) {
    const auto scope =
        replica_contact_scope(source, *reinterpret_cast<void**>(movement + 504), movement);
    if (scope) {
      const auto kind = slot<unsigned(__fastcall*)(void*)>(action, 14)(action);
      const bool bound = tracked->second.start(*scope, reinterpret_cast<std::uintptr_t>(action),
                                               kind, replaying_combat_input);
      log("COMBAT observer attack bind source=%u action=%p bound=%u", source, action,
          unsigned(bound));
    }
  }
  log_native_attack("start", action, result, 0);
  return result;
}
std::intptr_t __fastcall hooked_attack_exit(void* action) {
  auto movement = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(action) + 112);
  const auto tracked = replica_contacts.find(remote_combat_id(movement));
  if (tracked != replica_contacts.end()) {
    tracked->second.exit(reinterpret_cast<std::uintptr_t>(action));
  }
  ULONGLONG elapsed = 0;
  if (auto it = native_attack_started.find(action); it != native_attack_started.end()) {
    elapsed = GetTickCount64() - it->second;
    native_attack_started.erase(it);
  }
  log_native_attack("exit", action, 0, elapsed);
  return original_attack_exit(action);
}

}  // namespace kcds::game_client
