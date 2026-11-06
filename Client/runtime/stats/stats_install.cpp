// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "stats_install.hpp"

#include <cstdint>
#include <cstring>

#include "../core/client_state.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../riding/horse_capacity.hpp"
#include "../npcs/native_npcs.hpp"
#include "../npcs/npc_attack.hpp"
#include "../npcs/npc_health.hpp"
#include "../npcs/npc_walk.hpp"
#include "../npcs/real_movement.hpp"
#include "../riding/trample.hpp"
#include "MinHook.h"
#include "player_stats.hpp"

namespace kcds::game_client {

DWORD install_stats_and_npcs() {
  native_part_params_init = reinterpret_cast<PartParamsInit>(resolve_unique(
      "PartParamsInit", "C7 01 06 00 00 00 33 C0 C7 41 10 FF FF BF FF C7 41 28 FF FF BF FF"));
  damage_type_init = reinterpret_cast<DamageTypeInit>(resolve_unique(
      "DamageTypeInit",
      "40 53 48 83 EC 20 48 8B D9 C7 01 FF FF FF FF 48 83 C1 08 48 8D 15 ?? ?? ?? ??"));
  damage_type_free = reinterpret_cast<DamageTypeFree>(
      resolve_unique("DamageTypeFree",
                     "40 53 48 83 EC 20 48 8B D9 48 8B 49 30 48 83 E9 0C E8 ?? ?? ?? ?? 48 8B 4B "
                     "18 48 83 E9 0C E8 ?? ?? ?? ?? 48 8B 4B 08"));
  native_soul_stat = reinterpret_cast<SoulStat>(resolve_unique(
      "SoulDerivedStat", "48 89 5C 24 08 48 89 74 24 18 57 48 83 EC 30 49 8B F0 8B DA"));
  // A server horse's saddlebags keep to its carry limit: its soul's capacity.
  kcds::runtime::riding::horse_capacity::install(reinterpret_cast<void*>(native_soul_stat), log);
  kcds::runtime::stats::Natives stats_natives;
  stats_natives.derived_stat = reinterpret_cast<void*>(native_soul_stat);
  // The soul's vtable slot 88, GetStatLevelRelative.
  stats_natives.stat_level_relative = resolve_unique(
      "SoulStatLevelRelative",
      "40 53 55 56 57 48 83 EC 38 8A 44 24 68 49 8B F8 88 44 24 68 8B F2 0F 29 74 24 20 48 8B "
      "D9 4C 89 44 24 78 E8 ?? ?? ?? ?? 48 8B 9B A8 00 00 00");
  stats_natives.set_state =
      resolve_unique("SoulSetState",
                     "48 8B C4 48 89 58 08 48 89 70 10 57 48 83 EC 70 0F 29 70 E8 48 8B F1 0F 29 "
                     "78 D8 44 0F 29 40 C8 F3 44 0F 10 05 ?? ?? ?? ?? 8B DA 85 D2 75");
  stats_natives.damage_type_init = reinterpret_cast<void*>(damage_type_init);
  stats_natives.damage_type_free = reinterpret_cast<void*>(damage_type_free);
  // A state's maximum, whose first call is the RPG parameters' getter
  // GetStatLevel and GetSkillLevel.
  constexpr char kStateMax[] = "48 83 EC 28 85 D2 75 12 E8 ?? ?? ?? ??";
  stats_natives.state_max = resolve_unique("SoulStateMax", kStateMax);
  stats_natives.rpg_params = resolve_unique("RpgParamsOfStateMax", kStateMax, 9, 13);
  stats_natives.stat_level = resolve_unique(
      "SoulStatLevel", "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 30 49 8B D8 8B FA");
  stats_natives.skill_level =
      resolve_unique("SoulSkillLevel", "48 83 EC 38 8B C2 0F 57 DB 44 8B 8C");
  if (!kcds::runtime::stats::install(stats_natives, log)) {
    log("STATS natives missing; the player's stats would not reach the server");
    return 1;
  }
  // The NPCs' health on their bodies uses the same soul natives.
  kcds::runtime::npcs::health::Natives npc_health_natives;
  npc_health_natives.set_state = stats_natives.set_state;
  npc_health_natives.damage_type_init = stats_natives.damage_type_init;
  npc_health_natives.damage_type_free = stats_natives.damage_type_free;
  if (!kcds::runtime::npcs::health::install(npc_health_natives)) {
    log("NPC health natives missing; NPC bodies would not follow the server's health");
  }
  // Whether a soul fights, answered yes for an NPC replica's soul
  // (npcs/native_npcs hooked_soul_fights), so other players lock on to it.
  if (void* soul_fights = resolve_unique("NativeSoulFights",
                                         "40 53 48 83 EC 20 48 8B 01 33 D2 "
                                         "48 8B D9 FF 90 00 04 00 00")) {
    if (MH_CreateHook(soul_fights, reinterpret_cast<void*>(hooked_soul_fights),
                      reinterpret_cast<void**>(&original_soul_fights)) != MH_OK ||
        MH_EnableHook(soul_fights) != MH_OK) {
      log("NPC soul-fights hook failed; NPC replicas are no lock-on targets");
    }
  }
  // The combat actor an NPC replica gets for lock-on (npcs/native_npcs give_npc_combat_actor):
  // the combat module's factory, its release and the actor's list of
  // them.
  create_combat_actor = reinterpret_cast<CreateCombatActor>(resolve_unique(
      "NativeCreateCombatActor",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 33 DB 48 8B FA 39 1D ?? ?? ?? ??"));
  release_combat_actor = reinterpret_cast<ReleaseCombatActor>(
      resolve_unique("NativeReleaseCombatActor",
                     "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 48 8B DA 48 8B 0A 48 85 C9 74 12"));
  register_combat_actor = reinterpret_cast<RegisterCombatActor>(
      resolve_unique("NativeRegisterCombatActor",
                     "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 "
                     "33 ED 48 8D 41 08 48 8B D9 48 8B F2"));
  // Native IsInCombatMode's class getter, not component presence. Unknown
  // implementations retain conservative fast NPC synchronization without a native call.
  npc_combat_mode = reinterpret_cast<kcds::runtime::npcs::CombatModeQuery>(
      resolve_unique("NpcCombatMode", "48 8B 89 10 02 00 00 48 8B 01 48 FF 60 08"));
  // The NPC walk (npcs/npc_walk.hpp): the AI module's global as its manager getter
  // reads it, the agent of an entity, the movement request's constructor
  // and cleanup, the default path request's constructor,
  // and the manager's task start and stop.
  kcds::runtime::npcs::walk::Natives walk_natives;
  walk_natives.ai_module_global = static_cast<void**>(resolve_unique(
      "NpcWalkAiModule",
      "40 53 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 48 8B D9 48 8B 48 78 48 8B 01 FF 50 50", 9, 13));
  walk_natives.agent_of_entity =
      resolve_unique("NpcWalkAgent", "48 89 4C 24 08 48 83 EC 28 48 8B 02 48 8B CA FF 50 10");
  walk_natives.request_init =
      resolve_unique("NpcWalkRequest", "48 89 5C 24 08 57 48 83 EC 20 33 FF 48 89 11 48 89 79 08");
  walk_natives.request_cleanup = resolve_unique(
      "NpcWalkRequestCleanup",
      "40 53 48 83 EC 20 48 8B D9 48 81 C1 58 01 00 00 E8 ?? ?? ?? ?? 48 8D 8B 18 01 00 00");
  walk_natives.path_request_init = resolve_unique(
      "NpcWalkPathRequest",
      "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? C6 83 84 00 00 00 01");
  walk_natives.start_task = resolve_unique(
      "NpcWalkStart",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F2 48 8B F9 48 8B 12 E8 ?? ?? ?? ??");
  walk_natives.stop_tasks = resolve_unique(
      "NpcWalkStop", "48 89 5C 24 20 57 48 83 EC 60 41 8A D8 48 C7 44 24 58 00 00 00 00");
  // The request's callback builders, as the game's own walk request calls them:
  // the calls at +29 and +63 of that sequence.
  constexpr char kWalkCallbacks[] =
      "48 8D 05 ?? ?? ?? ?? 48 89 74 24 30 48 8D 54 24 20 48 89 44 24 20 48 8D 8D 98 00 00 00 "
      "E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 74 24 30 48 8D 54 24 20 48 89 44 24 20 "
      "48 8D 8D 58 01 00 00 E8 ?? ?? ?? ??";
  walk_natives.bind_finished = resolve_unique("NpcWalkOnFinished", kWalkCallbacks, 30, 34);
  walk_natives.bind_progress = resolve_unique("NpcWalkOnProgress", kWalkCallbacks, 64, 68);
  // The NPC attack (npcs/npc_attack.hpp): the AI script binding's AddPersonallyHostile
  // and RemovePersonallyHostile.
  kcds::runtime::npcs::attack::Natives attack_natives;
  attack_natives.add_hostile =
      resolve_unique("NpcAddHostile",
                     "48 89 5C 24 08 48 89 74 24 18 57 48 83 EC 30 48 8B 0D ?? "
                     "?? ?? ?? 48 8B F2 41 8B D0 49 8B D9 48 8B 01 FF 50 70");
  attack_natives.remove_hostile =
      resolve_unique("NpcRemoveHostile",
                     "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 0D ?? ?? ?? ?? "
                     "48 8B F2 41 8B D0 49 8B D9 48 8B 01 FF 50 70 48 85 C0");
  attack_natives.skirmish_manager = resolve_unique(
      "NpcSkirmishManager",
      "40 53 48 83 EC 20 48 8B 1D ?? ?? ?? ?? 90 48 85 DB 75 7F 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? "
      "?? 48 8B 1D ?? ?? ?? ?? 48 85 DB 75 5A 39 1D ?? ?? ?? ?? 75 05 E8 ?? ?? ?? ?? 48 8B 05 ?? "
      "?? ?? ?? 48 8D 54 24 30 BB E8 00 00 00");
  attack_natives.start_skirmish =
      resolve_unique("NpcStartSkirmish",
                     "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 "
                     "EC 40 48 8B F1 41 8A E9 48");
  attack_natives.activate = resolve_unique(
      "NpcActivate",
      "48 89 5C 24 10 57 48 83 EC 20 80 89 78 17 00 00 08 48 8B D9 E8 ?? ?? ?? ?? 48 "
      "8B 8B C0 00 00 00");
  attack_natives.set_active =
      resolve_unique("NpcSetActive",
                     "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 30 48 8B F1 45 0F B6 "
                     "F0 48 8B 0D ?? ?? ?? ?? 0F B6 EA 48 81 C1 08 07");
  attack_natives.game =
      resolve_unique("NpcGameSingleton",
                     "48 89 5C 24 40 E8 ?? ?? ?? ?? 48 8B 48 18 48 8B 01 FF 90 18 01 00 00 48 8B "
                     "08 48 8B 51 38 48 8B C8 FF D2 B2 AF",
                     6, 10);
  attack_natives.set_opponent =
      resolve_unique("NpcSetOpponent",
                     "48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 FF 15 ?? ?? ?? ?? 80 BB "
                     "7A 02 00 00 00 74 0F 48");
  attack_natives.set_target =
      resolve_unique("NpcSetTarget",
                     "48 83 EC 28 80 B9 7A 02 00 00 00 74 0C 48 8B 89 B8 03 00 "
                     "00 E8 ?? ?? ?? ?? 48 83 C4 28 C3");
  attack_natives.ensure_automation =
      resolve_unique("NpcEnsureAutomation",
                     "48 89 5C 24 10 57 48 83 EC 30 48 83 B9 48 02 00 00 00 48 8B D9 74 0B");
  attack_natives.enable_automation =
      resolve_unique("NpcEnableAutomation",
                     "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 30 80 79 40 00 48 8B F1 74 10");
  attack_natives.director_enter =
      resolve_unique("NpcDirectorEvent",
                     "48 8B C4 48 89 58 10 48 89 68 18 48 89 70 20 57 41 56 41 57 48 83 EC "
                     "30 0F 29 70 D8 8B FA 8B 41 34 48 8B D9");
  // Diagnostic: whether the game updates an attacking NPC's combat automation at all
  // (180 72FB58, the automation update; counted for the automation enable_automation logs).
  if (void* update = resolve_unique(
          "NpcAutomationUpdate",
          "40 53 48 83 EC 30 80 79 40 00 48 8B D9 0F 29 74 24 20 74 64 48 83 C1 50 E8")) {
    if (MH_CreateHook(update, reinterpret_cast<void*>(hooked_automation_update),
                      reinterpret_cast<void**>(&original_automation_update)) != MH_OK ||
        MH_EnableHook(update) != MH_OK) {
      log("NPC automation update hook failed");
    }
  }
  attack_natives.start_attacking =
      resolve_unique("NpcStartAttacking",
                     "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 80 B8 E0 00 00 00 02 75 04 "
                     "B0 01 EB 1C 33 D2 48 8B CB E8");
  attack_natives.combat_request =
      resolve_unique("NpcCombatRequest",
                     "48 8B C4 48 89 58 18 48 89 70 20 57 48 83 EC 30 48 8B 3D ?? ?? ?? ?? "
                     "48 8B F1 48 89 48 E8 48 8B CF 88 50 F0 48 8D 50 E8 44 88 40 F1 C6 40 "
                     "10 00 E8");
  attack_natives.refresh_attack =
      resolve_unique("NpcAttackRefresh",
                     "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 "
                     "EC 20 48 8B 41 08 4C 8B F1 40 32 FF 48 8B B0 F8 01 00 00 E8");
  attack_natives.melee_offense =
      resolve_unique("NpcMeleeOffense",
                     "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 55 41 56 48 8B EC 48 83 EC "
                     "40 83 79 20 00 48 8B F9 0F 85 ?? ?? ?? ?? 48 8B 59 08 48 8B CB E8");
  attack_natives.automation_attack =
      resolve_unique("NpcAutomationAttack",
                     "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 30 48 8B F9 40 32 F6 E8 "
                     "?? ?? ?? ?? 44 8B 47 64 48 8D 54 24 40 44 0F B6 C8");
  attack_natives.arm = resolve_unique(
      "NpcArm",
      "48 89 5C 24 08 57 48 83 EC 30 40 8A FA 48 8B D9 84 D2 75 3C E8 ?? ?? ?? ?? 48 8B "
      "4B 08 48 8B 01 FF 50 28");
  attack_natives.draw_weapon =
      resolve_unique("NpcDrawWeapon",
                     "40 53 48 83 EC 20 48 8B 89 F8 01 00 00 48 8B DA 48 8B 89 80 02 00 00 E8 "
                     "?? ?? ?? ?? 48 8B D3 48 8B 08 4C 8B 41 50");
  attack_natives.draw_weapon_set =
      resolve_unique("NpcDrawWeaponSet",
                     "40 53 48 83 EC 20 8B 41 40 41 B8 02 00 00 00 03 C0 48 8B D9 48 63 D0 "
                     "48 8B 54 D1 48 E8");
  attack_natives.defense_enable =
      resolve_unique("NpcDefenseEnable",
                     "48 89 5C 24 08 48 89 7C 24 10 55 48 8B EC 48 83 EC 30 48 8B 59 08 48 "
                     "8B F9 48 8D 0D ?? ?? ?? ?? 48 89 7D F0 48 89 4D F8");
  attack_natives.defense_schedule =
      resolve_unique("NpcDefenseSchedule",
                     "48 89 5C 24 08 57 48 83 EC 20 48 8B DA 4C 8B C2 48 8D 54 24 38 48 "
                     "8B F9 E8 ?? ?? ?? ?? 48 8B 44 24 38 48 8D 54 24 38 4C");
  // Its two calls: the choice (E8 at + 24) and the build (E8 at + 50).
  constexpr const char* kDefenseSchedule =
      "48 89 5C 24 08 57 48 83 EC 20 48 8B DA 4C 8B C2 48 8D 54 24 38 48 8B F9 E8 ?? ?? ?? ?? "
      "48 8B 44 24 38 48 8D 54 24 38 4C 8B C3 48 89 44 24 38 48 8B CF E8";
  attack_natives.defense_event =
      resolve_unique("NpcDefenseEvent",
                     "48 89 5C 24 08 57 48 83 EC 20 83 7C 24 50 04 49 8B D8 48 8B F9 74 0B 48 "
                     "8B 5C 24 30 48 83 C4 20 5F C3 49 8B 00 48 8B CB");
  attack_natives.defense_choose = resolve_unique("NpcDefenseChoose", kDefenseSchedule, 25, 29);
  attack_natives.defense_build = resolve_unique("NpcDefenseBuild", kDefenseSchedule, 51, 55);
  attack_natives.request_combat =
      resolve_unique("NpcCombatRequest",
                     "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 70 20 41 57 48 83 EC 20 "
                     "4C 8D 72 09 4C 8B FA 41 80 3E 00 48 8D 59 08 48 8B F9 74 2A");
  if (!kcds::runtime::npcs::attack::install(attack_natives)) {
    log("NPC attack natives missing; NPCs would not attack whom the server tells them");
  }
  if (!kcds::runtime::npcs::walk::install(walk_natives)) {
    log("NPC walk natives missing; NPCs would not walk where the server tells them");
  }
  // Issue 016: the simulator's NPC bodies walk with the game's real movement, not in jumps.
  kcds::runtime::npcs::real_movement::install(log);
  return 0;
}

}  // namespace kcds::game_client
