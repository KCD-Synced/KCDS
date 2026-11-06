// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "combat_targets.hpp"

#include <cstdint>
#include <cstring>

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../hud/combat_hud_hooks.hpp"
#include "../input/input_mode.hpp"
#include "../markers/marker_hooks.hpp"
#include "../replicas/remote_lifecycle.hpp"
#include "../riding/trample.hpp"
#include "MinHook.h"
#include "attack_factory.hpp"
#include "attack_observers.hpp"
#include "clinch_relay.hpp"
#include "combat_identity.hpp"
#include "combat_input.hpp"
#include "defence.hpp"
#include "held_hits.hpp"
#include "hit_feedback.hpp"
#include "women_targets.hpp"
#include "melee_contacts.hpp"
#include "native_actions.hpp"
#include "native_effects.hpp"
#include "player_damage.hpp"
#include "sync_attacks.hpp"

namespace kcds::game_client {

void resolve_combat_targets(StartupTargets& targets) {
  auto set_combat_target = resolve_unique("NativeSetCombatTarget",
                                          "48 83 EC 28 80 B9 7A 02 00 00 00 74 0C 48 8B 89 B8 03 "
                                          "00 00 E8 07 00 00 00 48 83 C4 28 C3 CC CC");
  // The game's bool-property setter: a replica's riposte request and window
  set_bool_property = reinterpret_cast<SetBoolProperty>(
      resolve_unique("NativeSetBoolProperty",
                     "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 40 8A F2 48 8B F9 38 51 08 74 "
                     "32 48 8B 59 10 88 51 08 E8 ?? ?? ?? ?? 48 8B D3 48 8D 4C 24 20 E8 ?? ?? ?? "
                     "?? 48 8D 4F 18 44 8A C6 48 8B D3 E8"));
  auto combat_input =
      resolve_unique("NativeCombatInput",
                     "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 60 0F 29 74 24 50 41 8B F8 0F 28 "
                     "F3 48 8B F2 48 8B D9 E8 ?? ?? ?? ?? 83 B8 A8 00 00 00 00");
  auto combat_eligibility =
      resolve_unique("NativeCombatEnemyEligibility",
                     "48 89 5C 24 08 57 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 49 8B D8 48 8B FA 48 8B "
                     "88 A8 00 00 00 83 79 64 00 75 32 E8 ?? ?? ?? ?? 48 8B 08 48 8B 51 60");
  auto produce_melee =
      resolve_unique("NativeMeleeProducer",
                     "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 "
                     "8B EC 48 83 EC 70 48 8B 41 08 48 8B F1 4D 8B E1 49 8B F8 4C 8B EA 4C 8B B8 "
                     "10 02 00 00 4D 8D B7 10 02 00 00 49 8B 06");
  auto player_hit_feedback =
      resolve_unique("NativePlayerHitFeedback",
                     "48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 E8 A7 37 EC FF 48 8B 8B C8 "
                     "0C 00 00 48 8B D7 48 8B 01 48 8B 5C 24 30 48 83 C4 20 5F 48 FF 60 20");
  base_actor_hit_feedback = reinterpret_cast<ActorHitFeedback>(
      resolve_unique("NativeActorHitFeedback",
                     "48 83 EC 28 83 7A 18 03 75 05 48 83 C4 28 C3 48 8B 89 80 02 00 00 E8 ?? ?? "
                     "?? ?? 45 33 C0 33 D2 48 8B 08 4C 8B 89 E8 00 00 00 48 8B C8 48 83 C4 28 "
                     "49 FF E1"));
  auto hit_reaction =
      resolve_unique("NativeHitReaction",
                     "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 30 "
                     "48 8B F9 4C 89 48 E8 49 8B F1 48 8B EA 4C 8B CA 48 81 C1 D8 01 00 00 48 8B "
                     "D7 49 8B D8 E8 ?? ?? ?? ?? 48 85 DB 74 7D");
  native_sync_hit_database = static_cast<std::uint8_t*>(
      resolve_unique("NativeSyncHitDatabase",
                     "48 83 EC 28 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 66 0F 6F 0D ?? ?? ?? ?? 48 "
                     "8D 05 E9 8C CB 02 33 C9 48 89 05 ?? ?? ?? ?? 0F 57 C0 48 89 0D",
                     7, 11));
  native_sync_attack_database = static_cast<std::uint8_t*>(
      resolve_unique("NativeSyncAttackDatabase",
                     "48 83 EC 28 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 66 0F 6F 0D ?? ?? ?? ?? 48 "
                     "8D 05 A9 94 CB 02 33 C9 48 89 05",
                     7, 11));
  auto choose_sync_attack =
      resolve_unique("NativeChooseSyncAttack",
                     "48 89 5C 24 08 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 "
                     "83 EC 60 45 33 ED 48 8D 79 08 41 F6 40 28 20");
  construct_sync_attack = reinterpret_cast<ConstructSyncAttack>(
      resolve_unique("NativeConstructSyncAttack",
                     "48 89 5C 24 10 57 48 83 EC 20 83 3D ?? ?? ?? ?? 00 48 8B FA 48 8B D9 75 05 "
                     "E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 8D 54 24 30 45 33 C0 B9 B0 00 00 00 "
                     "FF D0 8B 4C 24 30 F0 01 0D ?? ?? ?? ?? F0 81 05 ?? ?? ?? ?? B0 00 00 00 F0 "
                     "FF 05 ?? ?? ?? ?? 48 85 C0 74 2D 48 8B D7 48 8B C8 E8 2B 00 00 00"));
  native_clinch_ready = reinterpret_cast<ClinchReady>(resolve_unique(
      "NativeClinchReady",
      "48 8B C4 53 48 81 EC 80 00 00 00 0F 29 70 E8 48 8B D9 48 8B 49 08 0F 29 78 D8 "
      "44 0F 29 40 C8 44 0F 29 48 B8 E8 ?? ?? ?? ?? 84 C0 0F 85"));
  auto start_clinch = resolve_unique("NativeStartClinch",
                                     "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 58 "
                                     "4C 8B 65 68 4D 8B F9 89 4D CC 49 "
                                     "8B F8 48 8B 8A 10 02 00 00 4C 8B F2 48 81 C1 F0 00 00 00");
  native_clinch_kinds[0] = static_cast<const std::uint32_t*>(resolve_unique(
      "NativeGuardClinchKind",
      "80 78 19 00 75 76 B3 01 88 5F 19 3B 0D ?? ?? ?? ?? 8B 2D ?? ?? ?? ?? 0F 84", 19, 23));
  native_clinch_kinds[1] = static_cast<const std::uint32_t*>(resolve_unique(
      "NativeApproachClinchKind",
      "85 C0 75 51 48 8B 55 08 45 33 C9 8B 0D ?? ?? ?? ?? 4C 8B C6 48 C7 44 24 20 00 "
      "00 00 00 E8",
      13, 17));
  // Read-only checks: optional, the contact completion and the push log work without them.
  native_in_clinch_state = reinterpret_cast<MovementCheck>(
      resolve_unique("NativeInClinchState",
                     "48 83 EC 28 48 8B 89 10 02 00 00 48 83 C1 60 48 8B 01 FF 50 08 48 8D 54 24 "
                     "30 89 44 24 30 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 8A 40 4D 48 83 C4 28 C3"));
  native_push_available = reinterpret_cast<ClinchCheck>(resolve_unique(
      "NativePushAvailable",
      "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 48 8B 49 08 E8 ?? ?? ?? ?? 32 DB 84 C0 75 0D 32 C0 "
      "48 8B 5C 24 30 48 83 C4 20 5F C3 48 8B 4F 08 BA 02 00 00 00 E8 ?? ?? ?? ?? 84 C0 74 E1 38 "
      "5F 1C 75 11"));
  log("COMBAT clinch checks in_clinch_state=%u push_available=%u",
      unsigned(native_in_clinch_state != nullptr), unsigned(native_push_available != nullptr));
  auto make_attack = resolve_unique(
      "NativeMakeAttack",
      "48 8B C4 48 89 58 20 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 68 FE FF FF 48 81 EC 60 02 "
      "00 00 0F 29 70 B8 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 40 01 00 00 48 8B F9 45 8B E9 48 "
      "8B 49 08 41 8B D8 48 8B F2 48 8B 01 FF 90 A8 05 00 00");
  auto make_sync_hit =
      resolve_unique("NativeMakeSyncHit",
                     "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 83 3D ?? ?? ?? "
                     "?? 00 41 8A F1 48 8B 59 08 49 8B E8 48 8B FA 75 05 E8 ?? ?? ?? ?? 48 8B 05 "
                     "?? ?? ?? ?? 48 8D 54 24 30 45 33 C0 B9 C8");
  auto queue_combat_query =
      resolve_unique("NativeCombatReactionQuery",
                     "48 89 5C 24 08 57 48 81 EC 80 00 00 00 83 79 2C 00 40 8A FA 7F 3E 83 79 30 "
                     "00 7E 05 80 FA 01 74 33 48 8B 59 10 49 8B D0 48 89 4C 24 20");
  auto perfect_block_window =
      resolve_unique("NativePerfectBlockWindow",
                     "48 89 5C 24 08 57 48 83 EC 20 48 8B 41 08 48 8B F9 48 8B 98 88 03 00 00 48 "
                     "8B 01 FF 90 A0 00 00 00 45 33 C0");
  auto submit_rpg_event =
      resolve_unique("NativeRpgEventSubmit",
                     "48 89 5C 24 08 48 89 74 24 18 57 48 83 EC 20 48 8B DA 48 8B F9 48 85 D2 75 "
                     "04 32 C0 EB 58 48 8B CB E8 ?? ?? ?? ??");
  auto native_event_table = static_cast<void**>(resolve_unique(
      "NativeRpgEventTable",
      "48 8D 05 ?? ?? ?? ?? 48 89 01 48 8B 07 48 C7 07 00 00 00 00 48 89 41 08 48 C7 41 10 00 00 "
      "00 00 48 C7 41 18 00 00 00 00 48 C7 41 20 00 00 00 00 48 C7 41 28 00 00 00 00",
      3, 7));
  // Generic deleting destructor thunks have identical code. Resolve C_Event's
  // own vtable from its unique constructor, then use its verified v0.
  auto destroy_rpg_event = native_event_table ? native_event_table[0] : nullptr;
  auto melee_hit = resolve_unique(
      "NativeMeleeHit",
      "40 55 53 56 57 41 56 48 8D AC 24 60 FF FF FF 48 81 EC A0 01 00 00 48 8B 05 ?? ?? ?? ?? 48 "
      "33 C4 48 89 85 90 00 00 00 48 8B 05 ?? ?? ?? ?? 4C 8B F1 49 8B F8 48 8B F2 48 8B 88 B0 00 "
      "00 00 48 8B 01 FF 50 08 48 8B D7 48 8D 4C 24 28 E8 C3 00 00 00");
  if (melee_hit) {
    auto instruction = static_cast<std::uint8_t*>(melee_hit) + 39;
    if (instruction[0] == 0x48 && instruction[1] == 0x8b && instruction[2] == 0x05) {
      std::int32_t displacement;
      std::memcpy(&displacement, instruction + 3, 4);
      rpg_module_slot = reinterpret_cast<void**>(instruction + 7 + displacement);
    }
  }
  auto combat_damage = resolve_unique(
      "NativeCombatSoulDamage",
      "48 89 5C 24 18 55 56 57 48 8D 6C 24 E0 48 81 EC 20 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 "
      "48 89 45 10 48 8B DA 0F 57 C0 48 8B 51 78 48 8B F9 0F 2F 82 08 07 00 00");
  auto set_native_characteristic = resolve_unique(
      "NativeSetCharacteristic",
      "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 40 0F 29 70 E8 "
      "49 8B E9 8B F2 0F 28 F2 0F 29 78 D8 48 8B D9 F3 0F 10 BC B1 A8 01 00 00 0F 2E F7");
  auto player_ctor = resolve_unique("PlayerCtor",
                                    "48 89 5C 24 18 48 89 74 24 20 55 57 41 56 48 8B EC 48 83 EC "
                                    "20 48 8B D9 E8 ?? ?? ?? ?? 41 83 CE FF 48 8D 05");
  actor_init = resolve_unique("ActorInit",
                              "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? "
                              "48 8B CB E8 ?? ?? ?? ?? E8 ?? ?? ?? ??");
  actor_post_init = resolve_unique("ActorPostInit",
                                   "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B F1 E8 ?? ?? "
                                   "?? ?? 48 8B 06 48 8B CE FF 90 E8 06 00 00");
  actor_release = resolve_unique("ActorRelease",
                                 "40 53 48 81 EC B0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 "
                                 "84 24 A0 00 00 00 4C 8B 05 ?? ?? ?? ??");
  actor_activate = resolve_unique("ActorActivate",
                                  "40 53 48 83 EC 30 48 8B D9 E8 ?? ?? ?? ?? 48 8B 8B 58 0A 00 00 "
                                  "E8 ?? ?? ?? ?? 48 8B 8B D0 09 00 00");
  actor_event = resolve_unique("ActorEvent",
                               "48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 E8 ?? ?? ?? ?? 44 "
                               "8B 07 41 83 E8 05 0F 84 ?? ?? ?? ?? 41 83");
  actor_reset = resolve_unique(
      "ActorReset", "48 89 5C 24 10 57 48 83 EC 40 48 83 B9 D0 09 00 00 00 40 8A FA 48 8B D9");
  actor_tick = resolve_unique("ActorTick",
                              "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B CB 48 83 C4 20 5B "
                              "E9 01 00 00 00 CC 48 89 5C 24 08 48 89 6C");
  actor_hit = resolve_unique("ActorHit",
                             "48 83 EC 28 83 7A 18 03 75 05 48 83 C4 28 C3 48 8B 89 80 02 00 00 E8 "
                             "?? ?? ?? ?? 45 33 C0 33 D2");
  actor_deactivate = resolve_unique("ActorDeactivate",
                                    "40 53 48 83 EC 30 48 8B D9 48 8B 89 D0 09 00 00 48 85 C9 74 "
                                    "30 C7 44 24 28 00 00 00 00 48 8D 05");
  actor_update = resolve_unique("ActorUpdate",
                                "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 83 BB 68 06 00 00 00 "
                                "74 0C 48 8D 8B 08 0A 00 00 E8 ?? ?? ?? ??");
  create_native_cause = reinterpret_cast<CreateNativeCause>(
      resolve_unique("create_cause",
                     "48 89 5C 24 10 57 48 83 EC 20 83 3D 57 64 D7 04 00 48 8B DA 48 8B F9 0F 84 "
                     "81 41 B0 01 45 33 C0 48 8D 54 24 30 B9 98 00 00 00 FF 15 40 64 D7 04"));
  create_native_event = reinterpret_cast<CreateNativeEvent>(
      resolve_unique("create_event",
                     "48 89 5C 24 10 48 89 4C 24 08 57 48 83 EC 20 83 3D B6 1D AC 04 00 49 8B F8 "
                     "48 8B DA 0F 84 A8 86 88 01 45 33 C0 48 8D 54 24 30 41 8D 48 38 FF 15"));
  create_health_effect = reinterpret_cast<CreateValueEffect>(
      resolve_unique("health_effect",
                     "48 8B C4 48 89 58 10 48 89 70 18 48 89 48 08 57 48 83 EC 40 33 FF 0F 29 70 "
                     "E8 39 3D BC 71 83 04 0F 28 F3 49 8B D8 48 8B F2 74 60 48 8B 05 B2 71"));
  create_stamina_effect = reinterpret_cast<CreateValueEffect>(
      resolve_unique("stamina_effect",
                     "48 8B C4 48 89 58 10 48 89 70 18 48 89 48 08 57 48 83 EC 30 83 3D D5 70 83 "
                     "04 00 49 8B D8 0F 29 70 E8 48 8B F2 0F 28 F3 0F 84 56 B6 63 01 48 8B"));
  create_buff_effect = reinterpret_cast<CreateBuffEffect>(
      resolve_unique("buff_effect",
                     "48 8B C4 48 89 58 10 48 89 68 18 48 89 70 20 48 89 48 08 57 48 83 EC 50 33 "
                     "FF 49 8B F1 39 3D D9 0A AC 04 49 8B D8 48 8B EA 0F 84 65 74 88 01 48"));
  create_unconscious_params = reinterpret_cast<CreateUnconsciousParams>(
      resolve_unique("unconscious_params",
                     "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 83 3D DE AB 77 02 00 49 8B F8 "
                     "48 8B F2 48 8B D9 75 05 E8 FE 82 73 FD 45 33 C0 48 8D 54 24 30 41 8D"));
  create_injured_params = reinterpret_cast<CreateInjuredParams>(
      resolve_unique("NativeInjuredParams",
                     "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B D9 49 8B "
                     "F9 B9 18 00 00 00 49 8B F0 48 8B EA E8 F6 8F 51 FE 48 85 C0 74 35 44"));
  create_morale_params = reinterpret_cast<CreateMoraleParams>(
      resolve_unique("NativeMoraleParams",
                     "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 83 3D B9 8A 83 "
                     "04 00 49 8B F9 49 8B F0 48 8B EA 48 8B D9 74 6A 45 33 C0 48 8D 54 24"));
  native_injured_params_type = resolve_unique(
      "NativeInjuredParamsType",
      "48 8D 0D 4E 90 F4 01 44 8B 06 F3 0F 10 45 00 4C 8B 54 24 50 45 0F B6 1A 44 88 58 11", 3, 7);
  native_morale_params_type = resolve_unique(
      "NativeMoraleParamsType",
      "4C 8D 05 01 72 E2 02 8A 0E F3 0F 10 45 00 F3 0F 11 40 08 4C 89 00 88 48 0C 88 50 0D", 3, 7);
  auto modify_native_event =
      resolve_unique("NativeEventModifiers",
                     "48 89 5C 24 18 55 56 57 41 56 41 57 48 83 EC 20 48 8B 69 18 48 8B F1 48 8B "
                     "79 10 EB 7E 4C 8B 3F 8A 5E 30 49 8B CF 49 8B 07 FF 50 28 48 8D 54 24");
  auto queue_native_hit =
      resolve_unique("NativeQueueHit",
                     "48 83 EC 28 4C 8D 89 D8 01 00 00 49 8B 49 08 49 3B 49 10 74 12 E8 AA 69 2B "
                     "03 49 81 41 08 A0 00 00 00 48 83 C4 28 C3 4C 8B C2 48 8B D1 49 8B C9");
  // soul RPG object vfunc 44, creates the RPG event of an actor collision.
  auto create_collision_hit_event =
      resolve_unique("NativeCollisionHitEvent",
                     "48 89 5C 24 08 55 56 57 48 8D AC 24 00 FE FF FF 48 81 EC 00 03 00 00 48 8B "
                     "05 ?? ?? ?? ?? 48 33 C4 48 89 85 F0 01 00 00 48 8B 05 ?? ?? ?? ?? 49 8B F8 "
                     "48 8B F2 48 8B 88 B0 00 00 00 48 8B 01 FF 50 08 48 8B D7 48 8D 4C 24 20 E8");
  auto interactive_state =
      resolve_unique("interactive_state",
                     "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F9 49 8B F0 41 8B 09 48 "
                     "8B DA 83 E9 03 0F 84 F2 00 00 00 83 E9 01 0F 84 8E 00 00 00 81 E9 81");
  auto move_to_start =
      resolve_unique("move_to_start",
                     "48 89 5C 24 10 55 48 8D AC 24 C0 FE FF FF 48 81 EC 40 02 00 00 48 8B 05 DC "
                     "15 E5 01 48 33 C4 48 89 85 30 01 00 00 48 8B D9 48 8D 4C 24 20 E8 C9");
  auto move_to_end =
      resolve_unique("move_to_end",
                     "40 53 48 83 EC 20 48 8B D9 3B 51 34 75 55 44 88 41 40 B2 01 C7 41 24 00 00 "
                     "00 00 C7 41 34 00 00 00 00 48 8B 49 38 48 8B 01 FF 90 38 04 00 00 E8");
  auto enable_action_map =
      resolve_unique("enable_action_map",
                     "48 89 5C 24 08 48 89 6C 24 18 56 57 41 56 48 83 EC 30 45 8A F0 48 8B F2 48 "
                     "8B E9 48 85 D2 0F 84 2E 2B 9F 01 80 3A 00 0F 84 25 2B 9F 01 48 8D 4C");
  create_hit_action = reinterpret_cast<CreateHitAction>(resolve_unique(
      "NativeCreateHitAction", "48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 41 56"));
  push_combat_action = reinterpret_cast<PushCombatAction>(
      resolve_unique("NativePushCombatAction", "40 53 48 83 EC 20 48 8B 99 00 02 00 00"));
  hit_action_variant = reinterpret_cast<HitActionVariant>(
      resolve_unique("NativeHitActionVariant", "45 84 C0 74 ?? 8B 05 ?? ?? ?? ?? 83 FA 05"));
  set_combat_int_property = reinterpret_cast<SetCombatIntProperty>(
      resolve_unique("NativeCombatIntProperty",
                     "48 83 EC 38 44 8B 41 08 44 8B CA 48 8B D1 45 3B C1 74 0D 44 89 49 08 48 83 "
                     "C1 18 E8 ?? ?? ?? ?? 48 83 C4 38 C3"));
  targets.set_combat_target = set_combat_target;
  targets.combat_input = combat_input;
  targets.combat_eligibility = combat_eligibility;
  targets.produce_melee = produce_melee;
  targets.player_hit_feedback = player_hit_feedback;
  targets.hit_reaction = hit_reaction;
  targets.choose_sync_attack = choose_sync_attack;
  targets.start_clinch = start_clinch;
  targets.make_attack = make_attack;
  targets.make_sync_hit = make_sync_hit;
  targets.queue_combat_query = queue_combat_query;
  targets.perfect_block_window = perfect_block_window;
  targets.submit_rpg_event = submit_rpg_event;
  targets.destroy_rpg_event = destroy_rpg_event;
  targets.melee_hit = melee_hit;
  targets.combat_damage = combat_damage;
  targets.set_native_characteristic = set_native_characteristic;
  targets.player_ctor = player_ctor;
  targets.modify_native_event = modify_native_event;
  targets.queue_native_hit = queue_native_hit;
  targets.create_collision_hit_event = create_collision_hit_event;
  targets.interactive_state = interactive_state;
  targets.move_to_start = move_to_start;
  targets.move_to_end = move_to_end;
  targets.enable_action_map = enable_action_map;
}

DWORD resolve_neutral_combat_targets(StartupTargets& targets) {
  auto combat_entry = resolve_unique("NativeNeutralCombatEntry",
                                     "48 89 5C 24 08 57 48 83 EC 20 48 8B B9 F8 03 00 00 48 8B D9 "
                                     "48 8B CF E8 74 00 00 00 84 C0 0F 85 70 62 64 01");
  native_body_combat_entry = reinterpret_cast<BodyCombatEntry>(
      resolve_unique("NativeBodyCombatEntry",
                     "48 89 5C 24 08 48 89 7C 24 10 55 48 8B EC 48 83 EC 70 48 8B 01 48 8B D9 8B "
                     "FA 48 8B 48 38 48 8B 01 FF 90 78 02 00 00 4C 8B C0"));
  native_animation_progress = reinterpret_cast<AnimationProgress>(resolve_unique(
      "NativeAnimationWholeClipProgress",
      "48 8B C4 48 89 58 08 48 89 68 18 48 89 70 20 57 41 54 41 55 41 56 41 57 48 83 EC 40 0F 29 "
      "70 C8 48 8B DA 0F 29 78 B8 48 8B F9 48 85 D2 75 0D F3 0F 10 05 ?? ?? ?? ??"));
  if (!native_animation_progress) {
    log("Native animation progress signature missing");
    return 1;
  }
  auto attack_start =
      resolve_unique("NativeAttackStartLifecycle",
                     "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 48 83 79 58 00 48 8B F2 0F 29 "
                     "74 24 30 48 8B F9 75 07 32 C0 E9 ?? ?? ?? ?? E8 ?? ?? ?? ??");
  auto attack_exit =
      resolve_unique("NativeAttackExitLifecycle",
                     "48 89 5C 24 08 57 48 83 EC 30 48 8B 59 70 48 8D 05 ?? ?? ?? ?? 48 89 44 24 "
                     "28 48 8B F9 E8 ?? ?? ?? ?? 48 8D 54 24 20 48 89 44 24 20");
  auto master_riposte_start = resolve_unique(
      "NativeMasterRiposteStart",
      "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 30 48 8B 81 88 00 00 00 48 8B F9 48 8B 89 D0 "
      "00 00 00 48 8B B0 10 02 00 00 48 8B 01 FF 50 48");
  if (!attack_start || !attack_exit || !master_riposte_start) {
    log("Native attack lifecycle diagnostic signatures missing");
    return 1;
  }
  auto actor_has_channel = resolve_unique(
      "NativeActorHasChannel",
      "48 83 EC 28 48 8B 49 28 48 8B 01 FF 90 80 00 00 00 66 85 C0 0F 95 C0 48 83 C4 28 C3");
  auto concept_map_marker =
      resolve_unique("NativeConceptMapMarker",
                     "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 40 48 8B D9 48 81 "
                     "C1 90 00 00 00 E8 1D 30 77 00 48 8B F8 48 85 C0 0F 84 B0 00 00 00");
  auto attack_space = resolve_unique(
      "NativeNeutralAttackSpace",
      "48 89 5C 24 08 48 89 7C 24 10 55 48 8D 6C 24 A9 48 81 EC A0 00 00 00 48 8B D9 48 8B FA 48 "
      "8D 4D E7 E8 ?? ?? ?? ?? 48 8B 4B 08 E8 ?? ?? ?? ?? F3 0F 5D 87 6C 01 00 00");
  auto show_denial_attack =
      resolve_unique("NativeShowDenialAttack",
                     "48 89 5C 24 08 57 48 83 EC 60 48 8B 0D ?? ?? ?? ?? 4C 8D 05 ?? ?? ?? ?? 48 "
                     "8D 54 24 78 48 8B 01 FF 50 70 48 8D 4C 24 78 48 8B 38 E8 ?? ?? ?? ?? 48 85 "
                     "FF 74 39 48 8B 07 48 8D 4C 24 30 48 8B 98 28 02 00 00 E8 ?? ?? ?? ?? 45 33 "
                     "C9 48 C7 44 24 20 00 00 00 00 4C 8B C0 48 8D 15 ?? ?? ?? ?? 48 8B CF FF D3");
  auto denied_ui_emit =
      resolve_unique("NativeDeniedUiEmit",
                     "48 89 5C 24 08 88 54 24 10 57 48 83 EC 20 48 8B F9 48 8D 54 24 38 48 83 C1 "
                     "38 E8 ?? ?? ?? ?? 48 8B D8 F3 0F 10 40 04 0F 2F 00 73 04 32 C0 EB 23");
  auto restriction_setter =
      resolve_unique("NativeMovementRestrictionSetter",
                     "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 0F "
                     "B6 C2 48 8B D9 44 38 44 08 18 0F 84 92 00 00 00 44 88 44 08 18 48 83 C1 18 "
                     "45 33 C0 48 8D 51 15 E8 0A 4E DA 00");
  if (!restriction_setter)
    return 1;
  auto sync_attack_update =
      resolve_unique("NativeSyncAttackUpdate",
                     "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 80 B9 A8 00 00 00 00 48 8B D9 "
                     "0F 29 74 24 20 0F 28 F1 74 32 48 8B 51 70");
  auto sync_attack_exit =
      resolve_unique("NativeSyncAttackExit",
                     "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D B1 A0 00 00 00 48 8B F9 "
                     "48 8B 1E 48 85 DB 74 20 BA 02 00 00 00 48 8B CB E8");
  auto hud_combat_input =
      resolve_unique("NativeHudCombatInput",
                     "48 83 EC 28 E8 ?? ?? ?? ?? 48 8B 80 F8 00 00 00 48 85 C0 74 15 48 8B 40 28 "
                     "48 85 C0 74 0C 48 8B 80 50 04 00 00 48 83 C4 28 C3 33 C0");
  auto denied_action_feedback = resolve_unique(
      "NativeDeniedActionFeedback",
      "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 48 8B DA 48 8B 49 08 48 8B 01 FF 10 4C 8B 00 4C 39 "
      "43 10 75 0D 0F B6 53 18 48 8B 4F 10 E8 ?? ?? ?? ?? 48 8B 5C 24 30 48 83 C4 20 5F C3");
  if (!show_denial_attack || !denied_ui_emit || !sync_attack_update || !sync_attack_exit ||
      !hud_combat_input || !denied_action_feedback || !attack_space || !actor_has_channel ||
      !concept_map_marker) {
    log("Native neutral selection/map concept signatures missing");
    return 1;
  }
  if (!combat_entry || !native_body_combat_entry) {
    log("Native neutral combat signatures missing");
    return 1;
  }
  targets.combat_entry = combat_entry;
  targets.attack_start = attack_start;
  targets.attack_exit = attack_exit;
  targets.master_riposte_start = master_riposte_start;
  targets.actor_has_channel = actor_has_channel;
  targets.concept_map_marker = concept_map_marker;
  targets.attack_space = attack_space;
  targets.show_denial_attack = show_denial_attack;
  targets.denied_ui_emit = denied_ui_emit;
  targets.restriction_setter = restriction_setter;
  targets.sync_attack_update = sync_attack_update;
  targets.sync_attack_exit = sync_attack_exit;
  targets.hud_combat_input = hud_combat_input;
  targets.denied_action_feedback = denied_action_feedback;
  return 0;
}

DWORD install_native_hit_hooks(const StartupTargets& targets) {
  // The targets under the names initialize_adapter() gave them.
  void* const modify_native_event = targets.modify_native_event;
  void* const queue_native_hit = targets.queue_native_hit;
  if (MH_CreateHook(modify_native_event, reinterpret_cast<void*>(hooked_modify_native_event),
                    reinterpret_cast<void**>(&original_modify_native_event)) != MH_OK ||
      MH_EnableHook(modify_native_event) != MH_OK)
    return 1;
  if (MH_CreateHook(queue_native_hit, reinterpret_cast<void*>(hooked_queue_native_hit),
                    reinterpret_cast<void**>(&original_queue_native_hit)) != MH_OK ||
      MH_EnableHook(queue_native_hit) != MH_OK)
    return 1;
  return 0;
}

DWORD install_combat_hooks(const StartupTargets& targets) {
  // The targets under the names initialize_adapter() gave them.
  void* const create_collision_hit_event = targets.create_collision_hit_event;
  void* const produce_melee = targets.produce_melee;
  void* const player_hit_feedback = targets.player_hit_feedback;
  void* const hit_reaction = targets.hit_reaction;
  void* const choose_sync_attack = targets.choose_sync_attack;
  void* const start_clinch = targets.start_clinch;
  void* const make_attack = targets.make_attack;
  void* const make_sync_hit = targets.make_sync_hit;
  void* const queue_combat_query = targets.queue_combat_query;
  void* const perfect_block_window = targets.perfect_block_window;
  void* const submit_rpg_event = targets.submit_rpg_event;
  void* const destroy_rpg_event = targets.destroy_rpg_event;
  void* const melee_hit = targets.melee_hit;
  void* const set_combat_target = targets.set_combat_target;
  if (MH_CreateHook(create_collision_hit_event,
                    reinterpret_cast<void*>(hooked_create_collision_hit_event),
                    reinterpret_cast<void**>(&original_create_collision_hit_event)) != MH_OK ||
      MH_EnableHook(create_collision_hit_event) != MH_OK)
    return 1;
  if (MH_CreateHook(produce_melee, reinterpret_cast<void*>(hooked_produce_melee),
                    reinterpret_cast<void**>(&original_produce_melee)) != MH_OK)
    return 1;
  if (MH_EnableHook(produce_melee) != MH_OK)
    return 1;
  if (MH_CreateHook(player_hit_feedback, reinterpret_cast<void*>(hooked_player_hit_feedback),
                    reinterpret_cast<void**>(&original_player_hit_feedback)) != MH_OK)
    return 1;
  if (MH_EnableHook(player_hit_feedback) != MH_OK)
    return 1;
  if (MH_CreateHook(hit_reaction, reinterpret_cast<void*>(hooked_hit_reaction),
                    reinterpret_cast<void**>(&original_hit_reaction)) != MH_OK)
    return 1;
  if (MH_EnableHook(hit_reaction) != MH_OK)
    return 1;
  if (MH_CreateHook(choose_sync_attack, reinterpret_cast<void*>(hooked_choose_sync_attack),
                    reinterpret_cast<void**>(&original_choose_sync_attack)) != MH_OK)
    return 1;
  if (MH_EnableHook(choose_sync_attack) != MH_OK)
    return 1;
  if (MH_CreateHook(start_clinch, reinterpret_cast<void*>(hooked_start_clinch),
                    reinterpret_cast<void**>(&original_start_clinch)) != MH_OK)
    return 1;
  if (MH_EnableHook(start_clinch) != MH_OK)
    return 1;
  if (MH_CreateHook(make_attack, reinterpret_cast<void*>(hooked_make_attack),
                    reinterpret_cast<void**>(&original_make_attack)) != MH_OK)
    return 1;
  if (MH_EnableHook(make_attack) != MH_OK)
    return 1;
  if (MH_CreateHook(make_sync_hit, reinterpret_cast<void*>(hooked_make_sync_hit),
                    reinterpret_cast<void**>(&original_make_sync_hit)) != MH_OK)
    return 1;
  if (MH_EnableHook(make_sync_hit) != MH_OK)
    return 1;
  if (MH_CreateHook(queue_combat_query, reinterpret_cast<void*>(hooked_queue_combat_query),
                    reinterpret_cast<void**>(&original_queue_combat_query)) != MH_OK)
    return 1;
  if (MH_EnableHook(queue_combat_query) != MH_OK)
    return 1;
  if (MH_CreateHook(perfect_block_window, reinterpret_cast<void*>(hooked_perfect_block_window),
                    reinterpret_cast<void**>(&original_perfect_block_window)) != MH_OK)
    return 1;
  if (MH_EnableHook(perfect_block_window) != MH_OK)
    return 1;
  if (MH_CreateHook(submit_rpg_event, reinterpret_cast<void*>(hooked_submit_rpg_event),
                    reinterpret_cast<void**>(&original_submit_rpg_event)) != MH_OK)
    return 1;
  if (MH_EnableHook(submit_rpg_event) != MH_OK)
    return 1;
  if (MH_CreateHook(destroy_rpg_event, reinterpret_cast<void*>(hooked_destroy_rpg_event),
                    reinterpret_cast<void**>(&original_destroy_rpg_event)) != MH_OK)
    return 1;
  if (MH_EnableHook(destroy_rpg_event) != MH_OK)
    return 1;
  if (MH_CreateHook(melee_hit, reinterpret_cast<void*>(hooked_melee_hit),
                    reinterpret_cast<void**>(&original_melee_hit)) != MH_OK)
    return 1;
  if (MH_EnableHook(melee_hit) != MH_OK)
    return 1;
  if (MH_CreateHook(set_combat_target, reinterpret_cast<void*>(hooked_set_combat_target),
                    reinterpret_cast<void**>(&original_set_combat_target)) != MH_OK)
    return 1;
  if (MH_EnableHook(set_combat_target) != MH_OK)
    return 1;
  return 0;
}

DWORD install_combat_input_hooks(const StartupTargets& targets) {
  // The targets under the names initialize_adapter() gave them.
  void* const combat_input = targets.combat_input;
  void* const combat_eligibility = targets.combat_eligibility;
  void* const combat_damage = targets.combat_damage;
  void* const set_native_characteristic = targets.set_native_characteristic;
  void* const player_ctor = targets.player_ctor;
  if (MH_CreateHook(combat_input, reinterpret_cast<void*>(hooked_combat_input),
                    reinterpret_cast<void**>(&original_combat_input)) != MH_OK)
    return 1;
  if (MH_EnableHook(combat_input) != MH_OK)
    return 1;
  if (MH_CreateHook(combat_eligibility, reinterpret_cast<void*>(hooked_combat_eligibility),
                    reinterpret_cast<void**>(&original_combat_eligibility)) != MH_OK)
    return 1;
  if (MH_EnableHook(combat_eligibility) != MH_OK)
    return 1;
  if (MH_CreateHook(combat_damage, reinterpret_cast<void*>(hooked_combat_damage),
                    reinterpret_cast<void**>(&original_combat_damage)) != MH_OK)
    return 1;
  if (MH_EnableHook(combat_damage) != MH_OK)
    return 1;
  if (MH_CreateHook(set_native_characteristic,
                    reinterpret_cast<void*>(hooked_set_native_characteristic),
                    reinterpret_cast<void**>(&original_set_native_characteristic)) != MH_OK)
    return 1;
  if (MH_EnableHook(set_native_characteristic) != MH_OK)
    return 1;
  if (MH_CreateHook(player_ctor, reinterpret_cast<void*>(hooked_player_ctor),
                    reinterpret_cast<void**>(&original_player_ctor)) != MH_OK)
    return 1;
  if (MH_EnableHook(player_ctor) != MH_OK)
    return 1;
  // Women players are lock-on targets (combat/women_targets.hpp); without it the lock-on only
  // leaves them out, as before.
  install_women_targets();
  return 0;
}

DWORD install_neutral_attack_hooks(const StartupTargets& targets) {
  // The targets under the names initialize_adapter() gave them.
  void* const attack_start = targets.attack_start;
  void* const attack_exit = targets.attack_exit;
  void* const master_riposte_start = targets.master_riposte_start;
  void* const show_denial_attack = targets.show_denial_attack;
  void* const denied_ui_emit = targets.denied_ui_emit;
  void* const restriction_setter = targets.restriction_setter;
  if (MH_CreateHook(attack_start, reinterpret_cast<void*>(hooked_attack_start),
                    reinterpret_cast<void**>(&original_attack_start)) != MH_OK ||
      MH_EnableHook(attack_start) != MH_OK)
    return 1;
  if (MH_CreateHook(attack_exit, reinterpret_cast<void*>(hooked_attack_exit),
                    reinterpret_cast<void**>(&original_attack_exit)) != MH_OK ||
      MH_EnableHook(attack_exit) != MH_OK)
    return 1;
  if (MH_CreateHook(master_riposte_start, reinterpret_cast<void*>(hooked_master_riposte_start),
                    reinterpret_cast<void**>(&original_master_riposte_start)) != MH_OK ||
      MH_EnableHook(master_riposte_start) != MH_OK)
    return 1;
  if (MH_CreateHook(show_denial_attack, reinterpret_cast<void*>(hooked_show_denial_attack),
                    reinterpret_cast<void**>(&original_show_denial_attack)) != MH_OK ||
      MH_EnableHook(show_denial_attack) != MH_OK)
    return 1;
  if (MH_CreateHook(denied_ui_emit, reinterpret_cast<void*>(hooked_denied_ui_emit),
                    reinterpret_cast<void**>(&original_denied_ui_emit)) != MH_OK ||
      MH_EnableHook(denied_ui_emit) != MH_OK)
    return 1;
  if (MH_CreateHook(restriction_setter, reinterpret_cast<void*>(hooked_set_movement_restriction),
                    reinterpret_cast<void**>(&original_set_movement_restriction)) != MH_OK ||
      MH_EnableHook(restriction_setter) != MH_OK)
    return 1;
  return 0;
}

DWORD install_neutral_combat_hooks(const StartupTargets& targets) {
  // The targets under the names initialize_adapter() gave them.
  void* const sync_attack_update = targets.sync_attack_update;
  void* const sync_attack_exit = targets.sync_attack_exit;
  void* const hud_combat_input = targets.hud_combat_input;
  void* const denied_action_feedback = targets.denied_action_feedback;
  void* const attack_space = targets.attack_space;
  void* const actor_has_channel = targets.actor_has_channel;
  void* const concept_map_marker = targets.concept_map_marker;
  void* const combat_entry = targets.combat_entry;
  if (MH_CreateHook(sync_attack_update, reinterpret_cast<void*>(hooked_sync_attack_update),
                    reinterpret_cast<void**>(&original_sync_attack_update)) != MH_OK ||
      MH_EnableHook(sync_attack_update) != MH_OK)
    return 1;
  if (MH_CreateHook(sync_attack_exit, reinterpret_cast<void*>(hooked_sync_attack_exit),
                    reinterpret_cast<void**>(&original_sync_attack_exit)) != MH_OK ||
      MH_EnableHook(sync_attack_exit) != MH_OK)
    return 1;
  if (MH_CreateHook(hud_combat_input, reinterpret_cast<void*>(hooked_hud_combat_input),
                    reinterpret_cast<void**>(&original_hud_combat_input)) != MH_OK ||
      MH_EnableHook(hud_combat_input) != MH_OK)
    return 1;
  if (MH_CreateHook(denied_action_feedback, reinterpret_cast<void*>(hooked_denied_action_feedback),
                    reinterpret_cast<void**>(&original_denied_action_feedback)) != MH_OK ||
      MH_EnableHook(denied_action_feedback) != MH_OK)
    return 1;
  if (MH_CreateHook(attack_space, reinterpret_cast<void*>(hooked_attack_space),
                    reinterpret_cast<void**>(&original_attack_space)) != MH_OK ||
      MH_EnableHook(attack_space) != MH_OK)
    return 1;
  if (MH_CreateHook(actor_has_channel, reinterpret_cast<void*>(hooked_actor_has_channel),
                    reinterpret_cast<void**>(&original_actor_has_channel)) != MH_OK ||
      MH_EnableHook(actor_has_channel) != MH_OK)
    return 1;
  if (MH_CreateHook(concept_map_marker, reinterpret_cast<void*>(hooked_concept_map_marker),
                    reinterpret_cast<void**>(&original_concept_map_marker)) != MH_OK ||
      MH_EnableHook(concept_map_marker) != MH_OK)
    return 1;
  if (MH_CreateHook(combat_entry, reinterpret_cast<void*>(hooked_combat_entry),
                    reinterpret_cast<void**>(&original_combat_entry)) != MH_OK ||
      MH_EnableHook(combat_entry) != MH_OK)
    return 1;
  return 0;
}

}  // namespace kcds::game_client
