// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "combat_natives_install.hpp"

#include <cstdint>
#include <cstring>

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../crime/pvp_crime.hpp"
#include "../hud/combat_hud_hooks.hpp"
#include "MinHook.h"
#include "attack_observers.hpp"
#include "defence.hpp"
#include "dodge_relay.hpp"
#include "held_hits.hpp"
#include "hit_reactions.hpp"
#include "melee_contacts.hpp"
#include "takedowns.hpp"
#include "native_effects.hpp"

namespace kcds::game_client {

void install_perfect_block_hooks() {
  // mov eax, [request id] at +6 (rel32 at +8), call StartDefence at +39 (rel32 at
  // +40).
  if (auto perfect_block = static_cast<std::uint8_t*>(
          resolve_unique("NativePerfectBlockStart",
                         "40 53 48 83 EC 30 8B 05 ?? ?? ?? ?? 48 8B D9 48 8B 49 08 44 8B CA 89 "
                         "44 24 28 48 8D 54 24 40 41 B0 01 C6 44 24 20 00 E8"))) {
    std::int32_t request_offset = 0;
    std::int32_t call_offset = 0;
    std::memcpy(&request_offset, perfect_block + 8, 4);
    std::memcpy(&call_offset, perfect_block + 40, 4);
    perfect_block_request = reinterpret_cast<const unsigned*>(perfect_block + 12 + request_offset);
    start_defence = reinterpret_cast<StartDefence>(perfect_block + 44 + call_offset);
    if (MH_CreateHook(reinterpret_cast<void*>(start_defence),
                      reinterpret_cast<void*>(hooked_start_defence),
                      reinterpret_cast<void**>(&original_start_defence)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(start_defence)) != MH_OK) {
      log("COMBAT PERFECT BLOCK hook failed; perfect blocks stay on their owner's screen");
    } else {
      log("COMBAT PERFECT BLOCK hooked request=%u", *perfect_block_request);
    }
    auto window = resolve_unique("NativePerfectBlockWindow",
                                 "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 "
                                 "8B 69 28 8B F2 48 8B 59 20 48 8B F9 48 3B DD 74 ?? 48 8B 41 08 "
                                 "4C 8B 80 10 02 00");
    if (!window ||
        MH_CreateHook(window, reinterpret_cast<void*>(hooked_window_lookup),
                      reinterpret_cast<void**>(&original_window_lookup)) != MH_OK ||
        MH_EnableHook(window) != MH_OK) {
      original_window_lookup = nullptr;
      log("COMBAT PERFECT BLOCK window hook failed; relayed perfect blocks need the window open");
    }
  } else {
    log("COMBAT PERFECT BLOCK start not found; perfect blocks stay on their owner's screen");
  }
}

void install_dodge_hooks() {
  // Dodges: the dodge request (the controller's movement at + 0x18, its lock
  // target core + 0xCF8, its input vectors at + 0x68 and + 0x70) and the dodge's creation
  // (the movement's + 0x3A8 + 0x68 action factory, the filter's target at + 0x38).
  request_dodge = reinterpret_cast<RequestDodge>(
      resolve_unique("NativeDodgeRequest",
                     "48 89 5C 24 10 55 48 8D 6C 24 A9 48 81 EC C0 00 00 00 48 8B 05 ?? ?? ?? ?? "
                     "48 33 C4 48 89 45 4F 48 8B 41 18 48 8B D9 48 8B 90 10 02 00 00 48 83 BA "
                     "F8 0C 00 00 00 0F 84 ?? ?? ?? ?? F3 0F 10 41 68"));
  if (auto create_dodge = resolve_unique(
          "NativeCreateDodge",
          "48 8B C4 48 89 58 10 48 89 68 18 48 89 70 20 57 41 56 41 57 48 83 EC 30 4C 8B F9 4C 8B "
          "F2 48 8B 89 A8 03 00 00 48 8D 50 08 49 8B E9 48 8B 49 68 E8 ?? ?? ?? ?? 48 8B 5C 24 50 "
          "48 8D 75 38")) {
    if (MH_CreateHook(create_dodge, reinterpret_cast<void*>(hooked_create_dodge),
                      reinterpret_cast<void**>(&original_create_dodge)) != MH_OK ||
        MH_EnableHook(create_dodge) != MH_OK) {
      original_create_dodge = nullptr;
      request_dodge = nullptr;
      log("COMBAT DODGE hook failed; dodges stay on their owner's screen");
    } else {
      log("COMBAT DODGE hooked request=%u", unsigned(request_dodge != nullptr));
    }
  } else {
    request_dodge = nullptr;
    log("COMBAT DODGE creation not found; dodges stay on their owner's screen");
  }
}

void resolve_contact_natives() {
  action_handles_hit = reinterpret_cast<ActionHandlesHit>(
      resolve_unique("NativeActionHandlesHit",
                     "48 89 5C 24 10 57 48 83 EC 20 48 8B 89 00 02 00 00 48 8D 54 24 30 45 33 "
                     "C0 E8 ?? ?? ?? ?? 48 8B 5C 24 30 48 85 DB 74 ?? 48 8B 03 48 8B CB FF 90 "
                     "10 01 00 00"));
  native_physical_contact = reinterpret_cast<PhysicalContact>(
      resolve_unique("NativePhysicalContact",
                     "48 89 5C 24 18 55 56 57 41 56 41 57 48 8D 6C 24 B0 48 81 EC 50 01 00 00 48 "
                     "8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 40 45 33 F6 48 8B FA 48 8D 54 24"));
  produce_native_hit = reinterpret_cast<ProduceNativeHit>(
      resolve_unique("NativeContactHit",
                     "48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 98 48 81 EC 68 01 "
                     "00 00 0F 29 70 A8 0F 29 78 98 48 8B 05 E6 86 BA 03"));
  // Unique original physical-contact postamble. Resolve its
  // actual call target, rather than a common float-property setter prologue.
  complete_physical_attack_power = reinterpret_cast<CompletePhysicalAttackPower>(resolve_unique(
      "NativePhysicalAttackPowerCompletion",
      "80 7D 30 00 48 8B CB 74 0D 0F 57 C9 E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? F3 0F 10 4D D0 EB EF", 13,
      17));
  set_native_contact_target = reinterpret_cast<SetNativeContactTarget>(resolve_unique(
      "NativeContactTargetProperty",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 40 8A F2 48 8B F9 38 51 08 74 32 48 8B "
      "59 10 88 51 08 E8 ?? ?? ?? ?? 48 8B D3 48 8D 4C 24 20"));
  native_health_effect_type =
      resolve_unique("NativeHealthEffectType",
                     "48 8D 05 AF 5A E2 02 48 89 03 89 4B 18 48 89 4B 20 88 4B 28", 3, 7);
  native_stamina_effect_type =
      resolve_unique("NativeStaminaEffectType",
                     "48 8D 05 9E 59 E2 02 F3 0F 11 77 1C 48 89 07 C7 47 18 01 00 00 00", 3, 7);
  native_buff_effect_type =
      resolve_unique("NativeBuffEffectType", "48 8D 05 59 C3 08 03 48 8B 5C 24 30 48 89 07", 3, 7);
}

void install_hit_pricing() {
  // The game's price of another player's blow and its health effect (PricedBlow).
  // Without them a blow waits for its ruling before the game works it out.
  void* price_blow = resolve_unique("PriceBlow", "4C 8B DC 53 48 83 EC 40 33 C0 48 8B DA");
  void* apply_health = resolve_unique("ApplyHealthEffect",
                                      "4C 8B DC 49 89 5B 10 49 89 6B 18 49 89 73 20 57 48 83 "
                                      "EC 30 48 8B 41 10");
  cap_price = reinterpret_cast<CapPrice>(
      resolve_unique("CapPrice", "48 89 5C 24 08 57 48 83 EC 20 80 B9 92 00 00 00"));
  calculator_item_32 = reinterpret_cast<CalculatorItem>(resolve_unique(
      "CalculatorItem32", "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B 4B 20 48 8D"));
  calculator_item_48 = reinterpret_cast<CalculatorItem>(resolve_unique(
      "CalculatorItem48", "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B 4B 30 48 8D"));
  void* knock_out =
      resolve_unique("LethalKnockOut", "48 89 5C 24 10 F3 0F 11 54 24 18 57 48 83 EC 30");
  combat_config = reinterpret_cast<CombatConfig>(resolve_unique(
      "CombatConfig",
      "48 83 EC 28 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA 18 00 00 00 48 8B 04 C8 8B 04 "
      "02 39 05 ?? ?? ?? ?? 7F 0C 48 8D 05 ?? ?? ?? ?? 48 83 C4 28 C3 48 8D 0D ?? ?? ?? ?? E8 ?? "
      "?? ?? ?? 83 3D ?? ?? ?? ?? FF 75 DF E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? EB "
      "CC CC 40 53 48 83"));
  priced_hits_ready =
      price_blow && apply_health && cap_price && calculator_item_32 && calculator_item_48 &&
      knock_out && combat_config &&
      MH_CreateHook(price_blow, reinterpret_cast<void*>(hooked_price_blow),
                    reinterpret_cast<void**>(&original_price_blow)) == MH_OK &&
      MH_CreateHook(apply_health, reinterpret_cast<void*>(hooked_apply_health_effect),
                    reinterpret_cast<void**>(&original_apply_health_effect)) == MH_OK &&
      MH_CreateHook(knock_out, reinterpret_cast<void*>(hooked_knock_out),
                    reinterpret_cast<void**>(&original_knock_out)) == MH_OK &&
      MH_EnableHook(price_blow) == MH_OK && MH_EnableHook(apply_health) == MH_OK &&
      MH_EnableHook(knock_out) == MH_OK;
  log("HIT pricing hooks %s", priced_hits_ready ? "ready" : "missing: blows wait unpriced");
}

void install_trample_diagnostics() {
  struct DiagnosticHook {
    const char* name;
    const char* pattern;
    void* detour;
    void** original;
  };
  const DiagnosticHook hooks[] = {
      {"ActorHitReaction",
       "48 89 5C 24 18 55 56 57 41 56 41 57 48 8D 6C 24 C9 48 81 EC B0 00 00 00 F6 42 78 04",
       reinterpret_cast<void*>(hooked_actor_hit_reaction),
       reinterpret_cast<void**>(&original_actor_hit_reaction)},
      {"HitReactionQuery",
       "40 55 53 56 57 41 56 48 8D 6C 24 C9 48 81 EC B0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 "
       "48 89 45 27 45 33 F6",
       reinterpret_cast<void*>(hooked_hit_reaction_query),
       reinterpret_cast<void**>(&original_hit_reaction_query)},
      {"AnimationReaction", "48 8B C4 48 89 58 08 48 89 70 18 4C 89 48 20 48 89 50 10",
       reinterpret_cast<void*>(hooked_animation_reaction),
       reinterpret_cast<void**>(&original_animation_reaction)},
      {"ReactionDatabaseQuery",
       "48 89 5C 24 08 48 89 74 24 18 48 89 7C 24 20 55 48 8B EC 48 81 EC 80 00 00 00 48 8B 05 "
       "?? ?? ?? ?? 48 33 C4 48 89 45 F0 33 F6",
       reinterpret_cast<void*>(hooked_reaction_database_query),
       reinterpret_cast<void**>(&original_reaction_database_query)},
      {"SourceHitReport", "48 83 C1 18 E9 ?? ?? ?? ?? CC CC CC 48 8B C4 48 89 58 08 48 89 70 10",
       reinterpret_cast<void*>(hooked_source_hit_report),
       reinterpret_cast<void**>(&original_source_hit_report)},
  };
  for (const DiagnosticHook& hook : hooks) {
    void* target = resolve_unique(hook.name, hook.pattern);
    const bool installed = target && MH_CreateHook(target, hook.detour, hook.original) == MH_OK &&
                           MH_EnableHook(target) == MH_OK;
    log("COMBAT TRAMPLE diagnostic hook %s %s", hook.name, installed ? "installed" : "missing");
  }
}

namespace {

// A soul's WUID (C_Soul + 48), the id the game's skirmish participants are kept by.
constexpr std::size_t kSoulWuid = 48;
constexpr std::size_t kActorSoul = 1640;

std::uint64_t soul_wuid(const void* soul) {
  std::uint64_t wuid = 0;
  if (soul) {
    std::memcpy(&wuid, static_cast<const std::uint8_t*>(soul) + kSoulWuid, sizeof(wuid));
  }
  return wuid;
}

// Whether a skirmish participant is the local player or another player's replica.
bool is_player_wuid(std::uint64_t id) {
  if (!id || !multiplayer_world.load()) {
    return false;
  }
  if (auto actor = static_cast<const std::uint8_t*>(local_combat_actor.load())) {
    void* local_soul = nullptr;
    std::memcpy(&local_soul, actor + kActorSoul, sizeof(local_soul));
    if (soul_wuid(local_soul) == id) {
      return true;
    }
  }
  std::lock_guard<std::mutex> lock(combat_identity_mutex);
  for (const auto& [soul, owner] : remote_combat_souls) {
    if (soul_wuid(soul) == id) {
      return true;
    }
  }
  return false;
}

}  // namespace

void install_optional_combat_guards() {
  // Optional: without it a takedown on a server NPC or another player crashes the game.
  log("COMBAT takedown guard %s", install_takedown_guard() ? "installed" : "unavailable");
  // Optional: without it two players fighting near NPCs bring guards on the one whose game saw it
  // The GetAllSkirmishParticipants core.
  void* participants = resolve_unique(
      "SkirmishParticipants",
      "40 55 53 56 57 41 56 48 8D 6C 24 C9 48 81 EC A0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 "
      "48 89 45 27 49 8B F9 49 8B F0 48 8B DA E8 ?? ?? ?? ?? E8");
  log("CRIME players-only fights report no crime: %s",
      kcds::runtime::crime::pvp_crime::install(participants, is_player_wuid, log) ? "installed"
                                                                                  : "missing");
  // Optional: without it a UI-blocked defender still takes locked-on hits.
  if (auto contact_allowed = resolve_unique(
          "PlayerContactAllowed",
          "40 53 56 57 48 83 EC 20 48 8B F2 48 8B F9 48 85 D2 74 27 E8 ?? ?? ?? ?? 84 C0 74 1E 48 "
          "8B 8F 50 04 00 00 41 B0 01 33 D2 48 8B 49 20 E8 ?? ?? ?? ?? 48 3B C6 0F 84")) {
    const bool installed =
        MH_CreateHook(contact_allowed, reinterpret_cast<void*>(hooked_player_contact_allowed),
                      reinterpret_cast<void**>(&original_player_contact_allowed)) == MH_OK &&
        MH_EnableHook(contact_allowed) == MH_OK;
    log("COMBAT UI-blocked near miss override installed=%d", int(installed));
  } else {
    log("COMBAT UI-blocked near miss override signature missing");
  }
  // Optional: without it a replica whose own pair choice was refused does not swing the ordinary
  // attack its owner's game made.
  if (auto attack_request_check = resolve_unique(
          "NativeAttackRequestCheck",
          "40 53 48 83 EC 20 4C 8B 51 18 48 8B DA 49 8B CA E8 ?? ?? ?? ?? 4D 8B 02 48 8B D0 "
          "49 8B CA 4D 8B 88 60 06 00 00 41 B0 01 41 FF D1")) {
    const bool installed =
        MH_CreateHook(attack_request_check, reinterpret_cast<void*>(hooked_attack_request_check),
                      reinterpret_cast<void**>(&original_attack_request_check)) == MH_OK &&
        MH_EnableHook(attack_request_check) == MH_OK;
    log("COMBAT replica pair mark check installed=%d", int(installed));
  } else {
    log("COMBAT replica pair mark check signature missing");
  }
  // Optional: without it a replica's clinch switches the local player's push key.
  if (auto combat_ui_push_key = resolve_unique(
          "NativeCombatUiPushKey",
          "48 89 5C 24 10 55 56 57 48 83 EC 30 48 8B F1 48 8B 49 08 48 8B 89 F0 03 00 00 E8 "
          "?? ?? ?? ?? 40 8A E8 3A 46 58 75 0D")) {
    const bool installed =
        MH_CreateHook(combat_ui_push_key, reinterpret_cast<void*>(hooked_combat_ui_push_key),
                      reinterpret_cast<void**>(&original_combat_ui_push_key)) == MH_OK &&
        MH_EnableHook(combat_ui_push_key) == MH_OK;
    log("HUD replica push key filter installed=%d", int(installed));
  } else {
    log("HUD replica push key filter signature missing");
  }
}

void resolve_item_effect_natives() {
  // C_ItemHealthValueEffect's factory; its vtable from the lea at + 116.
  constexpr char kItemHealthEffect[] =
      "48 8B C4 48 89 58 10 48 89 70 18 48 89 48 08 57 48 83 EC 30 83 3D ?? ?? ?? ?? 00 49 8B D8 "
      "0F 29 70 E8 48 8B F2 0F 28 F3 75 05 E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 8D 54 24 40 45 "
      "33 C0 41 8D 48 20 FF D0 8B 4C 24 40 48 8B F8 F0 01 0D ?? ?? ?? ?? F0 83 05 ?? ?? ?? ?? 20 "
      "F0 FF 05 ?? ?? ?? ?? 48 85 C0 74 35 4C 8B C3 48 8B D6 48 8B C8 E8 ?? ?? ?? ?? 48 8D 05 ?? "
      "?? ?? ?? F3 0F 11 77 18 48 89 07";
  // Other value effects' factories are the same code with their own vtable: the RTTI class picks.
  constexpr char kItemHealthEffectClass[] = ".?AVC_ItemHealthValueEffect@rpgmodule@wh@@";
  create_item_health_effect = reinterpret_cast<CreateValueEffect>(resolve_by_vtable_class(
      "NativeItemHealthEffect", kItemHealthEffect, 119, 123, kItemHealthEffectClass, false));
  native_item_health_effect_type = resolve_by_vtable_class(
      "NativeItemHealthEffectType", kItemHealthEffect, 119, 123, kItemHealthEffectClass, true);
  log("COMBAT item wear effect factory=%p type=%p", create_item_health_effect,
      native_item_health_effect_type);
  // C_ItemDestroyedEffect's constructor (its vtable from the lea at + 21), and
  // the one factory that calls it (the call at + 112).
  constexpr char kItemDestroyedConstructor[] =
      "48 89 5C 24 10 57 48 83 EC 20 49 8B D9 48 8B F9 E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 "
      "5F 18 48 89 07";
  constexpr char kItemDestroyedClass[] = ".?AVC_ItemDestroyedEffect@rpgmodule@wh@@";
  constexpr char kItemDestroyedEffect[] =
      "48 8B C4 48 89 58 10 48 89 68 18 48 89 70 20 48 89 48 08 57 48 83 EC 20 33 F6 49 8B D9 39 "
      "35 ?? ?? ?? ?? 49 8B F8 48 8B EA 75 05 E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 8D 54 24 "
      "30 45 33 C0 41 8D 48 38 FF D0 8B 4C 24 30 F0 01 0D ?? ?? ?? ?? F0 83 05 ?? ?? ?? ?? 38 "
      "F0 FF 05 ?? ?? ?? ?? 48 85 C0 74 14 4C 8B CB 4C 8B C7 48 8B D5 48 8B C8 E8 ?? ?? ?? ??";
  native_item_destroyed_effect_type =
      resolve_by_vtable_class("NativeItemDestroyedEffectType", kItemDestroyedConstructor, 24, 28,
                              kItemDestroyedClass, true);
  create_item_destroyed_effect = reinterpret_cast<CreateItemDestroyedEffect>(resolve_by_callee(
      "NativeItemDestroyedEffect", kItemDestroyedEffect, 113, 117,
      resolve_by_vtable_class("NativeItemDestroyedConstructor", kItemDestroyedConstructor, 24, 28,
                              kItemDestroyedClass, false)));
  log("COMBAT item break effect factory=%p type=%p", create_item_destroyed_effect,
      native_item_destroyed_effect_type);
  native_unconscious_params_type = resolve_unique(
      "NativeUnconsciousParamsType",
      "48 8D 15 0A 27 1E 01 8B 0F F3 0F 11 40 08 48 89 10 89 48 0C C6 40 10 00", 3, 7);
  native_backoff_attack_type = static_cast<const unsigned*>(resolve_unique(
      "NativeBackoffAttackType", "3B 05 84 D3 5C 04 74 70 80 3F 00 0F 84 87 00 00 00", 2, 6));
}

}  // namespace kcds::game_client
