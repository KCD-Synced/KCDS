// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "combat_hud_hooks.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"

namespace kcds::game_client {

// MCP: C_DeniedActionEffect+16 is the soul ID and
// +24 the original denial kind. Each actor subscriber routes into the same
// local UI effect service182CAE844. Keep original local feedback, omit only
// known remote actor presentation; gameplay effects are not filtered here.
// MCP has only the HUD cursor update caller.
// Its original global CombatModule+40 selector can point at a remote movement
// during that actor's native update. HUD reads the verified local input only.
HudCombatInput original_hud_combat_input = nullptr;
void* __fastcall hooked_hud_combat_input() {
  auto original = original_hud_combat_input();
  auto actor = static_cast<std::uint8_t*>(local_combat_actor.load());
  auto movement = actor ? *reinterpret_cast<std::uint8_t**>(actor + 632) : nullptr;
  auto controller = movement ? *reinterpret_cast<std::uint8_t**>(movement + 1104) : nullptr;
  auto input = controller ? *reinterpret_cast<std::uint8_t**>(controller + 40) : nullptr;
  if (!input || *reinterpret_cast<void**>(input + 24) != actor)
    return original;
  if (original != controller) {
    static ULONGLONG next = 0;
    const auto now = GetTickCount64();
    if (now >= next) {
      next = now + 1000;
      log("HUD cursor selector corrected original=%p local=%p actor=%p tick=%llu", original,
          controller, actor, now);
    }
  }
  return controller;
}
DeniedActionFeedback original_denied_action_feedback = nullptr;
std::intptr_t __fastcall hooked_denied_action_feedback(void* self, const void* effect) {
  auto actor = static_cast<std::uint8_t*>(local_combat_actor.load());
  if (actor && effect) {
    auto soul = *reinterpret_cast<std::uint8_t**>(actor + 1640);
    const auto affected =
        *reinterpret_cast<const std::uint64_t*>(static_cast<const std::uint8_t*>(effect) + 16);
    if (soul && affected != *reinterpret_cast<std::uint64_t*>(soul + 48)) {
      core::EntityId remote_id = 0;
      {
        std::lock_guard<std::mutex> lock(combat_identity_mutex);
        for (const auto& [remote, id] : remote_combat_souls)
          if (affected == *reinterpret_cast<const std::uint64_t*>(
                              static_cast<const std::uint8_t*>(remote) + 48)) {
            remote_id = id;
            break;
          }
      }
      if (remote_id) {
        log("HUD remote denied-action omitted player=%u kind=%u tick=%llu", remote_id,
            unsigned(*(static_cast<const std::uint8_t*>(effect) + 24)), GetTickCount64());
        return 0;
      }
    }
  }
  return original_denied_action_feedback(self, effect);
}
// is presentation-only denied-action event emission.
// Trace accepted UI feedback; preserve rate limiting and original result.
DeniedUiEmit original_denied_ui_emit = nullptr;
char __fastcall hooked_denied_ui_emit(void* self, char kind) {
  auto result = original_denied_ui_emit(self, kind);
  static std::atomic<unsigned> count{0};
  if (result && count.fetch_add(1) < 64) {
    log("HUD native denial kind=%u self=%p replay_input=%u replica_actor=%p caller=%p tick=%llu",
        unsigned(static_cast<unsigned char>(kind)), self, unsigned(replaying_combat_input),
        replicated_attack_actor, _ReturnAddress(), GetTickCount64());
    void* frames[8]{};
    const auto n = CaptureStackBackTrace(1, 8, frames, nullptr);
    for (unsigned i = 0; i < n; ++i) {
      auto pc = static_cast<std::uint8_t*>(frames[i]);
      log("HUD denial stack %u pc=%p IDA=0x%llx", i, frames[i],
          pc >= image && pc < image + image_size
              ? 0x180000000ULL + static_cast<unsigned long long>(pc - image)
              : 0ULL);
    }
  }
  return result;
}
// MCP is C_CombatPlayerController v4: direct global
// HUD ShowDenialAttack. B3EB40/914798 store its Movement at +24.
// It is presentation only, separate from the RPG denied-action service.
ShowDenialAttack original_show_denial_attack = nullptr;
std::intptr_t __fastcall hooked_show_denial_attack(void* controller) {
  auto movement =
      controller ? *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(controller) + 24)
                 : nullptr;
  const auto remote = remote_combat_id(movement);
  if (remote && *reinterpret_cast<void**>(movement + 1104) == controller) {
    log("HUD ShowDenialAttack remote omitted player=%u controller=%p tick=%llu", remote, controller,
        GetTickCount64());
    return 0;
  }
  log("HUD ShowDenialAttack local controller=%p tick=%llu", controller, GetTickCount64());
  return original_show_denial_attack(controller);
}
// (C_CombatPlayerUI, slot 6, updated every frame for each player
// movement): when its movement's push becomes available or ends (in combat, in its guard
// state, and its clinch's push time reached, clinch component + 28 or + 32) it switches the action
// attack_special (F) of the game's one action map "combat_locked" on or off, and remembers that at
// + 88. A replica is a C_Player with its own combat UI (made for every C_PlayerMovement
// ->), so its clinch switched the local player's F key: off when the
// replica's clinch ended first while the local player's own still allowed the push, on in a clinch
// the local player was not part of (the watcher got the clinch's F prompt). The replica's F
// is its owner's relayed input, replayed into its own combat input without the action map, so only
// the local player's UI switches the key.
CombatUiPushKey original_combat_ui_push_key = nullptr;
void __fastcall hooked_combat_ui_push_key(void* ui) {
  auto movement = ui ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(ui) + 8) : nullptr;
  if (const auto remote = remote_combat_id(movement)) {
    // Every frame per replica: logged at most every 10 s.
    static std::atomic<ULONGLONG> next_log{0};
    const auto now = GetTickCount64();
    auto due = next_log.load();
    if (now >= due && next_log.compare_exchange_strong(due, now + 10000)) {
      log("HUD replica push key switch omitted player=%u", remote);
    }
    return;
  }
  original_combat_ui_push_key(ui);
}

}  // namespace kcds::game_client
