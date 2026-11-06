#include "npc_attack.hpp"

#include "../knockout/knockout.hpp"

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "MinHook.h"

namespace kcds::runtime::npcs::attack {
namespace {

// IEntity slot 67: the entity's AI object; its first flag bit marks an AI actor, its AI id is
// at +40. CAIActor slot 103 sets the attention target (by AI id).
constexpr std::size_t kEntityAiObject = 536 / 8;
constexpr std::size_t kAiActorFlags = 8;
constexpr std::size_t kAiObjectId = 40;
constexpr std::size_t kAiSetAttentionTarget = 824 / 8;
// The NPC agent's combat helper (C_NPCCombatHelper, inside C_NPC) and its request slot.
constexpr std::size_t kCombatHelper = 3864;
constexpr std::size_t kRequestCombatSlot = 5;

using HostileBinding = void*(__fastcall*)(void* binding, void* handler, std::uint32_t npc,
                                          std::uint32_t target);
using Get = void*(__fastcall*)(void*);
using Get0 = void*(__fastcall*)();
using StartSkirmish = bool(__fastcall*)(void* manager, void* attacker, void* victim, bool force);
using SetAttentionTarget = void(__fastcall*)(void* ai, std::uint32_t target);

Natives natives;
bool installed = false;
// Attacking NPC soul -> the soul it attacks.
std::mutex pairs_mutex;
std::unordered_map<const void*, const void*> pairs;

// A server NPC's agent -> the WUID of the entity the server lets it fight (0 for none).
std::mutex permits_mutex;
std::unordered_map<const void*, std::uint64_t> fight_permits;
// A governed agent -> its NPC's network id, and NPC -> striker WUID -> when it struck.
std::unordered_map<const void*, std::uint32_t> governed_npcs;
std::unordered_map<std::uint32_t, std::unordered_map<std::uint64_t, ULONGLONG>> strikes;
std::atomic<unsigned> refused_requests{0};
// DIAG: requests let through and fights ended by forgive().
std::atomic<unsigned> allowed_requests{0};
std::atomic<std::uint64_t> last_allowed_target{0};
std::atomic<unsigned> forgiven_fights{0};

using RequestCombat = bool(__fastcall*)(void* helper, void* request);
RequestCombat original_request_combat = nullptr;

// Whether the agent's NPC was struck by `target` within kStruckFightMs (permits_mutex held).
bool struck_recently(const void* agent, std::uint64_t target) {
  const auto npc = governed_npcs.find(agent);
  if (npc == governed_npcs.end()) {
    return false;
  }
  const auto by_npc = strikes.find(npc->second);
  if (by_npc == strikes.end()) {
    return false;
  }
  const auto at = by_npc->second.find(target);
  return at != by_npc->second.end() && GetTickCount64() - at->second <= kStruckFightMs;
}

// the combat helper takes a fight request (the opponent's WUID at + 0). A governed
// agent's request for anyone but its permitted target is refused, as the helper refuses one while
// another is pending.
bool __fastcall hooked_request_combat(void* helper, void* request) {
  const void* agent = static_cast<std::uint8_t*>(helper) - kCombatHelper;
  std::uint64_t target = 0;
  std::memcpy(&target, request, sizeof(target));
  {
    std::lock_guard<std::mutex> lock(permits_mutex);
    const auto it = fight_permits.find(agent);
    if (it != fight_permits.end() && it->second != target && !struck_recently(agent, target)) {
      ++refused_requests;
      return false;
    }
  }
  ++allowed_requests;
  last_allowed_target = target;
  return original_request_combat(helper, request);
}

// The script function handler the bindings end with (their only use of it, its slot 11,
// EndFunction): a stand-in whose every slot returns nothing.
std::uintptr_t __fastcall end_function(void*) {
  return 0;
}
void* const handler_table[16] = {
    reinterpret_cast<void*>(&end_function), reinterpret_cast<void*>(&end_function),
    reinterpret_cast<void*>(&end_function), reinterpret_cast<void*>(&end_function),
    reinterpret_cast<void*>(&end_function), reinterpret_cast<void*>(&end_function),
    reinterpret_cast<void*>(&end_function), reinterpret_cast<void*>(&end_function),
    reinterpret_cast<void*>(&end_function), reinterpret_cast<void*>(&end_function),
    reinterpret_cast<void*>(&end_function), reinterpret_cast<void*>(&end_function),
    reinterpret_cast<void*>(&end_function), reinterpret_cast<void*>(&end_function),
    reinterpret_cast<void*>(&end_function), reinterpret_cast<void*>(&end_function)};
struct Handler {
  void* const* table = handler_table;
};

template <class Function>
Function slot(void* object, std::size_t index) {
  return reinterpret_cast<Function>((*static_cast<void***>(object))[index]);
}

// The entity's AI actor, null when it has none.
void* ai_actor_of(void* entity) {
  void* ai = entity ? slot<Get>(entity, kEntityAiObject)(entity) : nullptr;
  if (!ai || (*(static_cast<std::uint8_t*>(ai) + kAiActorFlags) & 1) == 0) {
    return nullptr;
  }
  return ai;
}

// NPC actor -> the share of incoming attacks its game defends against (engine thread).
std::unordered_map<const void*, float> defense_chances;
// The combat automations whose defence was switched on.
std::unordered_map<const void*, bool> defended;
std::uint32_t defense_roll = 0x9e3779b9u;

// A number in [0, 1), from a small xorshift (no shared random state of the game).
float next_roll() {
  defense_roll ^= defense_roll << 13;
  defense_roll ^= defense_roll >> 17;
  defense_roll ^= defense_roll << 5;
  return float(defense_roll & 0xffffff) / float(0x1000000);
}

DefenseCounts defense_counts;

using ScheduleDefense = void(__fastcall*)(void* defense, void* attack);
ScheduleDefense original_schedule_defense = nullptr;

// The defence's listener on its body's combat events: event 4 is an attack coming at
// the body; it schedules a defence when the attack passes the attack's own check (its vtable
// + 200) and none is pending. Counted for the fight log.
using DefenseEvent = void(__fastcall*)(void* defense, void* source, void* attack, void* data,
                                       int event);
DefenseEvent original_defense_event = nullptr;

void __fastcall hooked_defense_event(void* defense, void* source, void* attack, void* data,
                                     int event) {
  constexpr int kAttackComing = 4;
  constexpr std::size_t kAttackCheck = 200 / 8;
  ++defense_counts.events;
  if (event == kAttackComing && attack) {
    ++defense_counts.attacks;
    using Check = bool(__fastcall*)(void*);
    if ((*reinterpret_cast<Check**>(attack))[kAttackCheck](attack)) {
      ++defense_counts.checked;
    }
  }
  original_defense_event(defense, source, attack, data, event);
}

// The defence (automation + 280; its + 8 the combat actor, whose + 504 is the actor) defends
// against the attack only within the NPC's chance; every other body is the game's alone.
void __fastcall hooked_schedule_defense(void* defense, void* attack) {
  constexpr std::size_t kDefenseCombat = 8;
  constexpr std::size_t kCombatActor = 504;
  auto* combat =
      defense
          ? *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(defense) + kDefenseCombat)
          : nullptr;
  const void* actor = combat ? *reinterpret_cast<void**>(combat + kCombatActor) : nullptr;
  ++defense_counts.asked;
  const auto found = defense_chances.find(actor);
  if (found == defense_chances.end() || !natives.defense_choose || !natives.defense_build) {
    // The game's own choice (setCombatSkill not used).
    ++defense_counts.let_through;
    original_schedule_defense(defense, attack);
    return;
  }
  // As the scheduling does: the game chooses a defence into its S_ActionParams (kind
  // at + 8) and builds it. The NPC's chance only lets the game's choice through or
  // withholds it (0): a defence forced where the game chose none left the NPC in its hit state
  // (64) with its block raised, taking the full blow (a sword).
  // The kinds: 5 block, 6 perfect block, 7 sync perfect block, 8 master strike, 9 dodge.
  constexpr std::size_t kParamsKind = 8;
  constexpr std::int32_t kNoDefense = 0;
  using Choose = void*(__fastcall*)(void* defense, void** params, void* attack);
  using Build = void(__fastcall*)(void* defense, void** params, void* attack);
  void* params = nullptr;
  reinterpret_cast<Choose>(natives.defense_choose)(defense, &params, attack);
  const bool parries = next_roll() < found->second;
  if (params) {
    auto* kind = reinterpret_cast<std::int32_t*>(static_cast<std::uint8_t*>(params) + kParamsKind);
    defense_counts.game_kind = *kind;
    if (!parries) {
      *kind = kNoDefense;
    }
  }
  if (parries) {
    ++defense_counts.let_through;
  }
  reinterpret_cast<Build>(natives.defense_build)(defense, &params, attack);
}

}  // namespace

DefenseCounts take_defense_counts() {
  const DefenseCounts counts = defense_counts;
  defense_counts = DefenseCounts{};
  return counts;
}

void set_defense_chance(void* npc_actor, float chance) {
  if (!npc_actor) {
    return;
  }
  if (chance < 0.0f) {
    defense_chances.erase(npc_actor);
  } else {
    defense_chances[npc_actor] = chance > 1.0f ? 1.0f : chance;
  }
}

bool install(const Natives& given) {
  natives = given;
  installed = natives.add_hostile && natives.remove_hostile;
  if (natives.defense_schedule &&
      MH_CreateHook(natives.defense_schedule, reinterpret_cast<void*>(hooked_schedule_defense),
                    reinterpret_cast<void**>(&original_schedule_defense)) == MH_OK) {
    MH_EnableHook(natives.defense_schedule);
  }
  if (natives.defense_event &&
      MH_CreateHook(natives.defense_event, reinterpret_cast<void*>(hooked_defense_event),
                    reinterpret_cast<void**>(&original_defense_event)) == MH_OK) {
    MH_EnableHook(natives.defense_event);
  }
  const bool governed =
      natives.request_combat &&
      MH_CreateHook(natives.request_combat, reinterpret_cast<void*>(hooked_request_combat),
                    reinterpret_cast<void**>(&original_request_combat)) == MH_OK &&
      MH_EnableHook(natives.request_combat) == MH_OK;
  return installed && governed;
}

bool start(std::uint32_t npc_id, void* npc_entity, void* npc_soul, std::uint32_t target_id,
           void* target_entity, void* target_soul) {
  if (!installed) {
    return false;
  }
  bool started = false;
  if (natives.skirmish_manager && natives.start_skirmish && npc_soul && target_soul) {
    void* manager = reinterpret_cast<Get0>(natives.skirmish_manager)();
    started = manager && reinterpret_cast<StartSkirmish>(natives.start_skirmish)(manager, npc_soul,
                                                                                 target_soul, true);
  }
  Handler handler;
  reinterpret_cast<HostileBinding>(natives.add_hostile)(nullptr, &handler, npc_id, target_id);
  void* npc_ai = ai_actor_of(npc_entity);
  void* target_ai = ai_actor_of(target_entity);
  if (npc_ai && target_ai) {
    const auto target = *reinterpret_cast<const std::uint32_t*>(
        static_cast<const std::uint8_t*>(target_ai) + kAiObjectId);
    slot<SetAttentionTarget>(npc_ai, kAiSetAttentionTarget)(npc_ai, target);
  }
  return started;
}

// C_NPC + 6008: 8 once C_NPC::Activate ran, 2 while the agent's AI is on; + 6009: 1
// when the agent is meant to have its AI.
constexpr std::size_t kActivated = 6008;
constexpr std::size_t kActive = 6009;
constexpr std::uint8_t kActivatedBit = 8;
constexpr std::uint8_t kAiOnBit = 2;
constexpr std::uint8_t kWantedOnBit = 1;

Activation activate_brain(void* agent) {
  Activation result;
  if (!agent) {
    return result;
  }
  auto bytes = static_cast<std::uint8_t*>(agent);
  result.before = bytes[kActivated];
  if ((result.before & kActivatedBit) == 0 && natives.activate) {
    reinterpret_cast<void(__fastcall*)(void*)>(natives.activate)(agent);
  }
  if ((bytes[kActivated] & kAiOnBit) == 0 && natives.set_active) {
    // The AI the agent is meant to have (C_NPC + 6009 bit 0), which the game applies again when it
    // refreshes the agent: set as C_NPC::Activate sets it, a brain switched off before.
    bytes[kActive] |= kWantedOnBit;
    reinterpret_cast<char(__fastcall*)(void*, std::uint8_t, char)>(natives.set_active)(agent, 1, 0);
  }
  result.after = bytes[kActivated];
  result.active = bytes[kActive];
  return result;
}

Activation deactivate_brain(void* agent) {
  Activation result;
  if (!agent) {
    return result;
  }
  auto bytes = static_cast<std::uint8_t*>(agent);
  result.before = bytes[kActivated];
  bytes[kActive] &= static_cast<std::uint8_t>(~kWantedOnBit);
  if ((bytes[kActivated] & kAiOnBit) != 0 && natives.set_active) {
    reinterpret_cast<char(__fastcall*)(void*, std::uint8_t, char)>(natives.set_active)(agent, 0, 0);
  }
  result.after = bytes[kActivated];
  result.active = bytes[kActive];
  return result;
}

void govern(void* agent, std::uint32_t npc) {
  if (!agent) {
    return;
  }
  std::lock_guard<std::mutex> lock(permits_mutex);
  fight_permits.try_emplace(agent, 0);
  governed_npcs[agent] = npc;
}

bool fight_back_hit(const std::uint8_t* hit) {
  // A push counts (hit type 10, no damage): the game's own guards answer a player who keeps
  // shoving them by fighting, and refused here a shoved guard only staggered.
  // Not in the seconds after a revive: getting up pushes whoever stands there, and the bandit took
  // the revived player on again. A body running into the NPC (a collision) never counts.
  constexpr unsigned kCollisionType = 3;
  constexpr unsigned kPushType = 10;
  constexpr std::uint64_t kGettingUpMs = 5000;
  if (!hit) {
    return false;
  }
  const unsigned type = *reinterpret_cast<const unsigned*>(hit + 24);
  if (type == kCollisionType) {
    return false;
  }
  if (type == kPushType) {
    const std::uint64_t revived = kcds::runtime::knockout::revived_at();
    return revived == 0 || GetTickCount64() - revived > kGettingUpMs;
  }
  return true;
}

void struck_by(std::uint32_t npc, std::uint64_t striker) {
  if (!striker) {
    return;
  }
  std::lock_guard<std::mutex> lock(permits_mutex);
  strikes[npc][striker] = GetTickCount64();
}

void permit_fight(void* agent, std::uint64_t target) {
  if (!agent) {
    return;
  }
  std::lock_guard<std::mutex> lock(permits_mutex);
  fight_permits[agent] = target;
}

void forgive(std::uint64_t striker) {
  if (!striker) {
    return;
  }
  std::vector<void*> fighting;
  {
    std::lock_guard<std::mutex> lock(permits_mutex);
    for (auto& [npc, strikers] : strikes) {
      if (strikers.erase(striker) == 0) {
        continue;
      }
      for (const auto& [agent, governed] : governed_npcs) {
        const auto permit = fight_permits.find(agent);
        if (governed == npc && (permit == fight_permits.end() || permit->second != striker)) {
          fighting.push_back(const_cast<void*>(agent));
        }
      }
    }
  }
  for (void* agent : fighting) {
    end_fight(agent);
    ++forgiven_fights;
  }
}

void forget_agent(void* agent) {
  std::lock_guard<std::mutex> lock(permits_mutex);
  fight_permits.erase(agent);
  const auto npc = governed_npcs.find(agent);
  if (npc != governed_npcs.end()) {
    strikes.erase(npc->second);
    governed_npcs.erase(npc);
  }
}

unsigned take_refused_requests() {
  return refused_requests.exchange(0);
}

RequestStats take_request_stats() {
  RequestStats stats;
  stats.allowed = allowed_requests.exchange(0);
  stats.last_target = last_allowed_target.load();
  stats.forgiven = forgiven_fights.exchange(0);
  return stats;
}

bool request_combat(void* agent, std::uint64_t target) {
  if (!agent || !target) {
    return false;
  }
  // The request as the combat node builds it: the opponent's WUID, no relation
  // override, the hardcoded 1, 0, and no result callback (its function left empty, which the
  // helper skips).
  alignas(16) std::uint8_t request[96] = {};
  std::memcpy(request, &target, sizeof(target));
  request[9] = 1;
  void* helper = static_cast<std::uint8_t*>(agent) + kCombatHelper;
  using RequestCombat = bool(__fastcall*)(void* helper, void* request);
  return slot<RequestCombat>(helper, kRequestCombatSlot)(helper, request);
}

bool face_opponent(void* npc_actor, void* target_actor) {
  constexpr std::size_t kActorCombat = 632;
  constexpr std::size_t kCombatOwner = 504;
  constexpr std::size_t kOwnerHandle = 56;
  if (!npc_actor || !target_actor || !natives.set_opponent || !natives.set_target) {
    return false;
  }
  auto combat_of = [](void* actor) {
    return *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(actor) + kActorCombat);
  };
  std::uint8_t* npc_combat = combat_of(npc_actor);
  std::uint8_t* target_combat = combat_of(target_actor);
  if (!npc_combat || !target_combat) {
    return false;
  }
  // The target's handle, as every caller of the setters takes it.
  auto* owner = *reinterpret_cast<std::uint8_t**>(target_combat + kCombatOwner);
  void* handle = owner ? *reinterpret_cast<void**>(owner + kOwnerHandle) : nullptr;
  if (!handle) {
    return false;
  }
  using Set = void(__fastcall*)(void* combat, void* handle);
  reinterpret_cast<Set>(natives.set_opponent)(npc_combat, handle);
  reinterpret_cast<Set>(natives.set_target)(npc_combat, handle);
  return true;
}

namespace {

// The combat automation's attack (automation + 360) of the NPC's body, null while it has none.
std::uint8_t* automation_attack_of(void* npc_actor) {
  constexpr std::size_t kActorCombat = 632;
  constexpr std::size_t kCombatAutomation = 584;
  constexpr std::size_t kAutomationAttack = 360;
  auto* combat =
      npc_actor
          ? *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(npc_actor) + kActorCombat)
          : nullptr;
  auto* automation =
      combat ? *reinterpret_cast<std::uint8_t**>(combat + kCombatAutomation) : nullptr;
  return automation ? automation + kAutomationAttack : nullptr;
}

}  // namespace

bool ready_to_swing(void* npc_actor) {
  // The attack's state (+ 164) is 0 between attacks; its own update ends one.
  constexpr std::size_t kAttackState = 164;
  auto* attack = automation_attack_of(npc_actor);
  return attack && *reinterpret_cast<const std::int32_t*>(attack + kAttackState) == 0;
}

bool swing(void* npc_actor, std::int32_t zone) {
  constexpr std::size_t kAttackZone = 100;
  auto* attack = automation_attack_of(npc_actor);
  if (!attack || !natives.automation_attack || !ready_to_swing(npc_actor)) {
    return false;
  }
  *reinterpret_cast<std::int32_t*>(attack + kAttackZone) = zone;
  using Attack = bool(__fastcall*)(void* attack);
  return reinterpret_cast<Attack>(natives.automation_attack)(attack);
}

bool defending(void* npc_actor) {
  constexpr std::size_t kActorCombat = 632;
  constexpr std::size_t kCombatAutomation = 584;
  constexpr std::size_t kAutomationContext = 72;
  constexpr std::size_t kContextDefense = 264;
  auto* combat =
      npc_actor
          ? *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(npc_actor) + kActorCombat)
          : nullptr;
  auto* automation =
      combat ? *reinterpret_cast<std::uint8_t**>(combat + kCombatAutomation) : nullptr;
  auto* context =
      automation ? *reinterpret_cast<std::uint8_t**>(automation + kAutomationContext) : nullptr;
  return context && *reinterpret_cast<void**>(context + kContextDefense) != nullptr;
}

CombatSnapshot combat_snapshot(void* npc_actor) {
  // The combat actor (actor + 632; the defence's + 8, which the block action's switch takes) and
  // its core (+ 528).
  constexpr std::size_t kActorCombat = 632;
  constexpr std::size_t kCombatCore = 528;
  constexpr std::size_t kCoreState = 56;
  constexpr std::size_t kCoreBlock = 1584;
  CombatSnapshot snapshot;
  auto* combat =
      npc_actor
          ? *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(npc_actor) + kActorCombat)
          : nullptr;
  auto* core = combat ? *reinterpret_cast<std::uint8_t**>(combat + kCombatCore) : nullptr;
  if (!core || !*reinterpret_cast<void**>(core + kCoreBlock)) {
    return snapshot;
  }
  snapshot.state = *reinterpret_cast<const std::int32_t*>(core + kCoreState);
  using Get = char(__fastcall*)(void*);
  void* block = core + kCoreBlock;
  snapshot.block = (*reinterpret_cast<Get**>(block))[2](block);
  return snapshot;
}

bool draw_carried(void* npc_actor) {
  // The actor's weapon slots (its slot 191) drawing their current set, what the
  // script call human:DrawWeapon() does (C_ScriptBindHuman slot 14).
  constexpr std::size_t kActorWeaponSlots = 191;
  if (!npc_actor || !natives.draw_weapon_set) {
    return false;
  }
  using Get = void*(__fastcall*)(void*);
  void* weapons = slot<Get>(npc_actor, kActorWeaponSlots)(npc_actor);
  if (!weapons) {
    return false;
  }
  using DrawSet = void*(__fastcall*)(void* weapons);
  reinterpret_cast<DrawSet>(natives.draw_weapon_set)(weapons);
  return true;
}

Automation enable_automation(void* npc_actor, bool draw_once) {
  constexpr std::size_t kActorCombat = 632;
  constexpr std::size_t kCombatAutomation = 584;
  constexpr std::size_t kAutomationOn = 64;
  constexpr std::size_t kDirectorState = 132;
  Automation state;
  auto* combat =
      npc_actor
          ? *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(npc_actor) + kActorCombat)
          : nullptr;
  if (!combat || !natives.ensure_automation || !natives.enable_automation) {
    return state;
  }
  using Call = void(__fastcall*)(void*);
  reinterpret_cast<Call>(natives.ensure_automation)(combat);
  auto* automation = *reinterpret_cast<std::uint8_t**>(combat + kCombatAutomation);
  if (!automation) {
    return state;
  }
  if (!automation[kAutomationOn]) {
    reinterpret_cast<Call>(natives.enable_automation)(automation);
  }
  constexpr std::size_t kCombatCore = 528;
  constexpr std::size_t kCoreOpponent = 3320;
  auto* core = *reinterpret_cast<std::uint8_t**>(combat + kCombatCore);
  state.opponent = core ? *reinterpret_cast<void**>(core + kCoreOpponent) : nullptr;
  state.on = automation[kAutomationOn] != 0;
  state.director = *reinterpret_cast<const std::int32_t*>(automation + kDirectorState);
  // The director's own conditions to enter a fight, read for the log: its core
  // busy ([combat + 528] + 1200, slot 1), the context state ([automation + 72] + 224, not 2),
  // and the opponent's two combat states ([[opponent + 1000] + 8] + 528 + 2320, slot 1 with 1
  // and 0: 9, 10, 14, 15 or 13).
  if (state.opponent) {
    auto* busy = core + 1200;
    state.busy = slot<bool(__fastcall*)(void*)>(busy, 1)(busy);
    state.busy_table = *reinterpret_cast<void**>(busy);
    state.busy_check = (*reinterpret_cast<void***>(busy))[1];
    auto* context = *reinterpret_cast<std::uint8_t**>(automation + 72);
    state.context = context ? context[224] : -1;
    auto* opponent_owner =
        *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(state.opponent) + 1000);
    auto* opponent_combat =
        opponent_owner ? *reinterpret_cast<std::uint8_t**>(opponent_owner + 8) : nullptr;
    auto* opponent_core =
        opponent_combat ? *reinterpret_cast<std::uint8_t**>(opponent_combat + 528) : nullptr;
    if (opponent_core) {
      auto* machine = opponent_core + 2320;
      using StateOf = std::int32_t(__fastcall*)(void*, int);
      state.opponent_state_1 = slot<StateOf>(machine, 1)(machine, 1);
      state.opponent_state_0 = slot<StateOf>(machine, 1)(machine, 0);
    }
  }
  // The automation's melee offense (automation + 224): what the game's fight start
  // switches on for an NPC with a melee weapon. It takes an enable handle on the
  // automation (its + 32) and turns the automation's feature 0 on ([automation + 1328] + 24).
  // The director's own path (its states 2 and 7) is the bow's and refuses a sword.
  constexpr std::size_t kMeleeOffense = 224;
  constexpr std::size_t kOffenseHandle = 32;
  constexpr std::size_t kOffenseFeature = 1328 + 24;
  auto* offense = automation + kMeleeOffense;
  // Experiment (the sword): the offense starts only once a weapon is in hand, so it does not
  // take the fists' stance.
  const bool in_hand = !draw_once;
  if (state.on && state.opponent && in_hand && natives.melee_offense &&
      *reinterpret_cast<const std::int32_t*>(offense + kOffenseHandle) == 0) {
    using Start = void*(__fastcall*)(void* offense);
    reinterpret_cast<Start>(natives.melee_offense)(offense);
  }
  // The weapon in hand: what the script call human:DrawWeapon does (C_ScriptBindHuman slot 14),
  // the actor's weapon slots (its slot 191) drawing their current set.
  // The offense started without a weapon in hand takes the fists, so it waits for this.
  constexpr std::size_t kStance = 1224;
  constexpr std::size_t kStanceArmed = 96;
  // Once per fight: the core's drawn flag stays off with the sword in hand, and a second draw
  // put the sword away again.
  if (state.on && state.opponent && core && draw_once) {
    state.drew = draw_carried(*reinterpret_cast<void**>(combat + 504));
  }
  // Its defence (automation + 280): listens to the attacks coming at the body and blocks or
  // parries them as the game's combat brain decides (within the NPC's chance, above).
  constexpr std::size_t kDefense = 280;
  if (state.on && state.opponent && natives.defense_enable && !defended[automation]) {
    using Enable = void*(__fastcall*)(void* defense, char on);
    reinterpret_cast<Enable>(natives.defense_enable)(automation + kDefense, 1);
    defended[automation] = true;
  }
  // Its guard (C_CombatAutomationGuard, automation + 1224) raised once the weapon is in hand
  // (with on; its + 96 is set once it took): blocks need it.
  if (state.on && state.opponent && !draw_once && natives.arm &&
      !automation[kStance + kStanceArmed]) {
    using Arm = void*(__fastcall*)(void* guard, char on);
    reinterpret_cast<Arm>(natives.arm)(automation + kStance, 1);
  }
  state.armed = automation[kStance + kStanceArmed];
  // Whether a weapon (or the fists) is in hand: the combat core's slot 1.
  if (core) {
    state.drawn = slot<bool(__fastcall*)(void*)>(core, 1)(core);
  }
  state.entered = *reinterpret_cast<const std::int32_t*>(offense + kOffenseHandle);
  state.attacking = automation[kOffenseFeature];
  auto* context = *reinterpret_cast<std::uint8_t**>(automation + 72);
  if (context) {
    state.mode = context[204];
    state.context_192 = *reinterpret_cast<const std::int32_t*>(context + 244);
  }
  return state;
}

void end_fight(void* agent) {
  constexpr std::size_t kCombatHelper = 3864;
  constexpr std::size_t kHelperPending = 144;
  constexpr std::size_t kAbortSlot = 6;
  if (!agent) {
    return;
  }
  auto* helper = static_cast<std::uint8_t*>(agent) + kCombatHelper;
  if (!helper[kHelperPending]) {
    return;
  }
  slot<void(__fastcall*)(void*)>(helper, kAbortSlot)(helper);
}

bool wuid_known(std::uint64_t wuid) {
  constexpr std::size_t kWuidRegistry = 464;
  void* game = natives.game ? reinterpret_cast<Get0>(natives.game)() : nullptr;
  void* registry =
      game ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(game) + kWuidRegistry) : nullptr;
  using Find = void*(__fastcall*)(void* registry, std::uint64_t wuid);
  return registry && slot<Find>(registry, 0)(registry, wuid) != nullptr;
}

void mark(void* npc_soul, void* target_soul) {
  std::lock_guard<std::mutex> lock(pairs_mutex);
  pairs[npc_soul] = target_soul;
}

void unmark(void* npc_soul) {
  std::lock_guard<std::mutex> lock(pairs_mutex);
  pairs.erase(npc_soul);
}

bool enemies(const void* source, const void* target) {
  if (!source || !target) {
    return false;
  }
  std::lock_guard<std::mutex> lock(pairs_mutex);
  for (const auto& [npc, attacked] : pairs) {
    if ((npc == source && attacked == target) || (npc == target && attacked == source)) {
      return true;
    }
  }
  return false;
}

void stop(std::uint32_t npc_id, std::uint32_t target_id) {
  if (!installed) {
    return;
  }
  Handler handler;
  reinterpret_cast<HostileBinding>(natives.remove_hostile)(nullptr, &handler, npc_id, target_id);
}

}  // namespace kcds::runtime::npcs::attack
