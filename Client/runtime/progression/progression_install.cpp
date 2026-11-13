// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "progression_install.hpp"

#include <cstdint>
#include <cstring>

#include "../buffs/custom_buffs.hpp"
#include "../buffs/horse_buffs.hpp"
#include "../buffs/player_buffs.hpp"
#include "../buffs/replica_buffs.hpp"
#include "../combat/native_effects.hpp"
#include "../core/client_state.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "MinHook.h"
#include "perk_gate.hpp"
#include "progression_orders.hpp"
#include "replica_soul.hpp"
#include "xp_gate.hpp"

namespace kcds::game_client {

DWORD install_progression() {
  kcds::runtime::progression::OrderNatives order_natives;
  // AddSkillXP's own sequence: the RPG module, the script cause, the skill XP
  // effect; AddStatXP's the stat XP effect.
  constexpr char kSkillXpSite[] =
      "73 D3 48 8B 05 ?? ?? ?? ?? 48 8B 98 B0 00 00 00 48 8B CB 48 8B 03 FF 50 08 48 8B 46 30 "
      "4C 8D 44 24 30 48 8D 54 24 60 48 89 44 24 30 E8 ?? ?? ?? ?? 48 8B 03 48 8B CB FF 50 08 "
      "41 8B 06 44 8B CD 4C 8B 46 30 48 8B 54 24 60 89 84 24 80 00 00 00 48 8D 84 24 80 00 00 "
      "00 48 89 44 24 20 E8 ?? ?? ?? ??";
  constexpr char kStatXpSite[] =
      "73 D6 48 8B 05 ?? ?? ?? ?? 48 8B 98 B0 00 00 00 48 8B CB 48 8B 03 FF 50 08 48 8B 46 30 "
      "4C 8D 44 24 30 48 8D 54 24 60 48 89 44 24 30 E8 ?? ?? ?? ?? 48 8B 03 48 8B CB FF 50 08 "
      "41 8B 06 44 8B CD 4C 8B 46 30 48 8B 54 24 60 89 84 24 80 00 00 00 48 8D 84 24 80 00 00 "
      "00 48 89 44 24 20 E8 ?? ?? ?? ??";
  order_natives.rpg = static_cast<void* const*>(resolve_unique("RpgModule", kSkillXpSite, 5, 9));
  order_natives.create_cause = resolve_unique("RpgScriptCause", kSkillXpSite, 45, 49);
  order_natives.skill_xp = resolve_unique("RpgSkillXpEffect", kSkillXpSite, 94, 98);
  order_natives.stat_xp = resolve_unique("RpgStatXpEffect", kStatXpSite, 94, 98);
  order_natives.release_cause = resolve_unique(
      "RpgCauseRelease", "48 83 EC 28 48 8B 09 48 85 C9 74 06 48 8B 01 FF 50 20 48 83");
  // AdvanceToSkillLevel: the RPG parameters and a skill's total XP.
  constexpr char kSkillLevelSite[] =
      "73 D3 E8 ?? ?? ?? ?? 44 8B C7 48 8D 54 24 34 48 8B C8 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 44 "
      "8B C6 48 8D 54 24 30 48 8B C8 E8 ?? ?? ?? ??";
  order_natives.rpg_params = resolve_unique("RpgParams", kSkillLevelSite, 3, 7);
  order_natives.skill_total = resolve_unique("RpgSkillTotalXp", kSkillLevelSite, 19, 23);
  order_natives.stat_total = resolve_unique(
      "RpgStatTotalXp", "48 89 5C 24 08 57 48 83 EC 20 41 8B D8 48 8B FA 45 8B C1 48");
  order_natives.add_perk =
      resolve_unique("SoulAddPerk", "48 89 5C 24 20 55 56 57 41 56 41 57 48 8B EC 48 83 EC 50 0F");
  order_natives.remove_perk =
      resolve_unique("SoulRemovePerk", "48 89 5C 24 10 57 48 83 EC 30 48 8B F9 48 83 C1");
  order_natives.update_perks = resolve_unique(
      "SoulUpdatePerks",
      "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 20 48 83 79 58 "
      "00 48 8B");
  // The script binding's call of the buff manager getter, as the knock-out
  // resolves it.
  order_natives.buff_manager =
      resolve_unique("BuffManager",
                     "48 8B 4D B0 48 8D 55 D0 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 0F 28 45 D0 "
                     "4C 8D 45 C0 0F 28 4D E0",
                     14, 18);
  // The game's perk-point listeners of skill and stat XP.
  order_natives.skill_points =
      resolve_unique("SkillPerkPointsListener",
                     "48 89 5C 24 08 57 48 83 EC 20 48 8B 41 08 48 8B DA 48 8B F9 4C 8B 40 08 "
                     "49 8B 40 30 48 39 42 10 75 67 8B 42");
  order_natives.stat_points =
      resolve_unique("StatPerkPointsListener",
                     "48 89 5C 24 08 57 48 83 EC 20 48 8B 41 08 48 8B DA 48 8B F9 4C 8B 40 08 "
                     "49 8B 40 30 48 39 42 10 75 66 8B 42");
  // What the game's XP add does after it changed a track: the skill set's change
  // of a skill (its listeners, then the induced fencing), and a stat's change
  // as C_StatXPEffect::Apply calls it.
  order_natives.skill_changed = resolve_unique(
      "SkillChanged",
      "48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 48 8B 8F 18 01 00 00 8B D3 48 83 C1 58 E8");
  order_natives.stat_changed = resolve_unique(
      "StatChanged", "44 8B 47 18 E8 ?? ?? ?? ?? 8B 57 18 48 8D 4B 58 89 47 28 E8 ?? ?? ?? ??", 20,
      24);
  kcds::runtime::progression::xp_gate::Natives gate_natives;
  // C_SkillXPEffect::Apply and C_StatXPEffect::Apply (their vtable slot 3).
  gate_natives.skill_apply =
      resolve_unique("SkillXpEffectApply", "48 89 5C 24 18 55 56 57 48 83 EC 50 48 8B F9 48");
  gate_natives.stat_apply = resolve_unique(
      "StatXpEffectApply", "48 89 5C 24 18 55 56 57 48 8B EC 48 83 EC 60 48 8B F9 0F 29");
  gate_natives.image = reinterpret_cast<std::uintptr_t>(image);
  if (!kcds::runtime::progression::xp_gate::install(gate_natives, log)) {
    log("PROGRESSION xp gate natives missing; XP from play would not ask the server");
    return 1;
  }
  if (!kcds::runtime::progression::install(order_natives, log)) {
    log("PROGRESSION order natives missing; the server's progression orders would do nothing");
    return 1;
  }
  kcds::runtime::progression::replica_soul::Natives soul_natives;
  soul_natives.add_perk = order_natives.add_perk;
  soul_natives.remove_perk = order_natives.remove_perk;
  if (!kcds::runtime::progression::replica_soul::install(soul_natives, log)) {
    log("REPLICA SOUL natives missing; replicas would fight with default stats");
    return 1;
  }
  kcds::runtime::buffs::Natives buff_natives;
  buff_natives.manager = order_natives.buff_manager;
  // The buff manager's create, through its lookup of the buff's definition
  constexpr char kBuffCreate[] =
      "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 50 4C 8B F1 49 "
      "8B E8 49 8D 88 18 03 00 00 48 8B DA 4D 8B C1 48 8D 50 E8 49 8B F1 E8 ?? ?? ?? ?? 41 B8 "
      "10 00 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 4C 24 40 E8 ?? ?? ?? ?? 85 C0 0F 84 ?? ?? ?? ?? "
      "48 8D 54 24 40 E8 ?? ?? ?? ??";
  buff_natives.create = resolve_unique("BuffCreate", kBuffCreate);
  buff_natives.definition = resolve_unique("BuffDefinition", kBuffCreate, 93, 97);
  // The create's call of its constructor (manager, &out, soul, definition,
  // original definition, init parameters, &perk), for the resources' own buffs.
  constexpr char kBuffConstruct[] =
      "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 50 4C 8B F1 49 "
      "8B E8 49 8D 88 18 03 00 00 48 8B DA 4D 8B C1 48 8D 50 E8 49 8B F1 E8 ?? ?? ?? ?? 41 B8 "
      "10 00 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 4C 24 40 E8 ?? ?? ?? ?? 85 C0 0F 84 ?? ?? ?? ?? "
      "48 8D 54 24 40 E8 ?? ?? ?? ?? 48 8B F8 48 85 C0 0F 84 ?? ?? ?? ?? 41 B8 10 00 00 00 48 "
      "8D 54 24 40 48 8B CE E8 ?? ?? ?? ?? 85 C0 74 ?? 48 8B D6 E8 ?? ?? ?? ?? 48 8B C8 48 85 "
      "C0 74 ?? 48 8B 84 24 88 00 00 00 4C 8B CF 4C 8B C5 48 8B D3 0F 10 00 48 8D 44 24 40 48 "
      "89 44 24 30 48 8B 84 24 80 00 00 00 48 89 44 24 28 48 89 4C 24 20 49 8B CE F3 0F 7F 44 "
      "24 40 E8 ?? ?? ?? ??";
  buff_natives.construct = resolve_unique("BuffConstruct", kBuffConstruct, 206, 210);
  if (!buff_natives.construct) {
    log("BUFFS constructor missing; the resources' own buffs are not shown");
  }
  if (!kcds::runtime::buffs::install(buff_natives, log)) {
    log("BUFFS natives missing; players' status effects would not reach the server");
    return 1;
  }
  kcds::runtime::buffs::custom::set_logger(log);
  kcds::runtime::buffs::horses::set_logger(log);
  kcds::runtime::buffs::custom::set_string_from_text(
      resolve_unique("CryStringFromText",
                     "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 44 8B 05 ?? ?? "
                     "?? 04 48 8B F1 65 48 8B 04 25 58 00 00 00 48 83 CB FF B9 18 00 00 00"));
  kcds::runtime::buffs::replica::Natives replica_buff_natives;
  replica_buff_natives.manager = order_natives.buff_manager;
  // C_InjuredSoulBuffInstance's init: its flags from the parameters' ints 2, 3.
  replica_buff_natives.injured_init = resolve_unique(
      "InjuredBuffInit",
      "48 89 5C 24 10 56 57 41 56 48 83 EC 20 49 8B D0 4C 89 41 10 49 8B F1 48 8B F9 E8 ?? ?? ?? "
      "?? 48 8B 06 BA 02 00 00 00 48 8B CE FF 50 10 BA 03 00 00 00 48 8B CE 85 C0 0F 95 C0 88 "
      "87 E8 00 00 00");
  replica_buff_natives.create_injured_params = reinterpret_cast<void*>(create_injured_params);
  if (!replica_buff_natives.injured_init || !replica_buff_natives.create_injured_params) {
    log("REPLICA BUFFS injury natives missing; injuries are not mirrored with their numbers");
  }
  if (!kcds::runtime::buffs::replica::install(replica_buff_natives, log)) {
    log("REPLICA BUFFS natives missing; replicas would fight without their owner's buffs");
    return 1;
  }
  kcds::runtime::progression::perk_gate::Natives perk_natives;
  // The game's learn, as C_LearnPerkEffect::Apply calls it, with the perk
  // set's offered check, the perk definitions and their lookup it calls.
  constexpr char kLearnPerk[] =
      "48 8B C4 48 89 58 08 48 89 70 10 57 48 83 EC 30 0F 28 02 48 8B F2 48 8B F9 66 0F 7F 40 "
      "E8 48 8B 49 08 48 8D 50 E8 E8 ?? ?? ?? ?? 84 C0 74 7E 0F 28 06 48 8D 54 24 20 48 8D 0D "
      "?? ?? ?? ?? 66 0F 7F 44 24 20 E8 ?? ?? ?? ?? 48 85 C0 74 5F 8B 48 48";
  perk_natives.learn = resolve_unique("SoulLearnPerk", kLearnPerk);
  perk_natives.offered = resolve_unique("PerkSetOffers", kLearnPerk, 39, 43);
  perk_natives.definitions = resolve_unique("PerkDefinitions", kLearnPerk, 58, 62);
  perk_natives.definition = resolve_unique("PerkDefinition", kLearnPerk, 69, 73);
  perk_natives.add_perk = order_natives.add_perk;
  // Whether the perk set's add refuses a perk: owned, or excluded by the game.
  perk_natives.add_refused = resolve_unique(
      "PerkSetAddRefused", "48 8B C4 48 89 58 08 48 89 70 18 57 48 83 EC 30 0F 10 02 48");
  if (!kcds::runtime::progression::perk_gate::install(perk_natives, log)) {
    log("PROGRESSION perk gate natives missing; the perk screen would not ask the server");
    return 1;
  }
  return 0;
}

}  // namespace kcds::game_client
