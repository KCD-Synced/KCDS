// How the simulator's NPC body attacks whom the server told it (Npc.attack):
// the game's own AI-level hostility, as its script binding AI.AddPersonallyHostile
// sets it, and the target as the NPC's attention target (AI.SetAttentiontarget); the NPC's own
// brain then fights it with the game's combat. Engine thread only.
#pragma once

#include <cstdint>

namespace kcds::runtime::npcs::attack {

// The game's natives (WHGame 1.5.5), as game_client.cpp resolves them: the AI script binding's
// AddPersonallyHostile and RemovePersonallyHostile, both
// (binding, function handler, npc entity id, target entity id).
struct Natives {
  void* add_hostile = nullptr;
  void* remove_hostile = nullptr;
  // The skirmish manager's getter and its skirmish start (vtable slot 2:
  // manager, attacker soul, victim soul, force), which makes the two enemies in a new skirmish
  // the way the game's own fights begin.
  void* skirmish_manager = nullptr;
  void* start_skirmish = nullptr;
  // C_NPC::Activate (the NPC agent): what the NPC manager runs for the NPCs a level
  // brings, and a body spawned at runtime may miss, so its brain never runs.
  void* activate = nullptr;
  // The NPC agent's AI switch (agent, on, quiet): C_NPC + 6008 bit 1, and the agent's
  // registration with the AI system. Live a runtime body's switch was off (flags 0x29).
  void* set_active = nullptr;
  // The game's singleton getter: its + 464 is the registry that finds an entity by
  // its WUID (slot 0), as the NPC combat helper resolves its opponent.
  void* game = nullptr;
  // A combat actor's opponent and target setters (combat actor, the other
  // combat actor's handle), as the game's combat automation picks a target.
  void* set_opponent = nullptr;
  void* set_target = nullptr;
  // A combat actor's automation (its + 584, C_CombatAutomation): made on demand and
  // switched on (sets its + 64), after which its director fights the opponent. Off by
  // default: a runtime NPC never swung, blocked or turned.
  void* ensure_automation = nullptr;
  void* enable_automation = nullptr;
  // The automation director's entry into a fight (the director at automation + 80):
  // true when it took the opponent.
  void* director_enter = nullptr;
  // The attack automation's start (automation + 1376): what the director's fight
  // state 7 runs when its timer ends; true when it attacks (its context state 2).
  void* start_attacking = nullptr;
  // A combat actor's request (combat actor, kind, on), queued for the combat module
  // as the director's fight start does (kind 3) to bring the body into its fighting mode.
  void* combat_request = nullptr;
  // The attack automation's context refresh (automation + 1376): reads the body's
  // weapons into the context (+ 192/196/200: 3 when none) and its armed state (+ 224).
  void* refresh_attack = nullptr;
  // The automation's melee offense start (automation + 224), as the game's fight
  // start runs it for an NPC armed for melee.
  void* melee_offense = nullptr;
  // The combat automation's own attack (automation + 360): makes an attack in the zone
  // at its + 100 with the combat actor's attack factory ([combat + 936] + 80) and starts it.
  void* automation_attack = nullptr;
  // The automation stance's arming (automation + 1224, on): asks the body to draw its
  // weapon and raises the guard ([combat + 528] + 2904).
  void* arm = nullptr;
  // A combat actor's weapon draw or holster (combat actor, {draw, slot, quiet}).
  void* draw_weapon = nullptr;
  // An actor's weapon slots drawing their current set (the weapon slots, the actor's
  // slot 191), as the script call human:DrawWeapon() does.
  void* draw_weapon_set = nullptr;
  // The combat automation's defence (C_CombatAutomationDefense, automation + 280): its switch
  // (defence, on), which listens to the attacks coming at the body, and its scheduling
  // of a defence against one (defence, the attack), hooked for the NPC's chance.
  void* defense_enable = nullptr;
  void* defense_schedule = nullptr;
  // The two steps of that scheduling: the game's choice (defence, params out, attack)
  // and the defence it builds from it (defence, params, attack).
  void* defense_choose = nullptr;
  void* defense_build = nullptr;
  // The NPC combat helper's fight request (its slot 5: helper, request), what the
  // brain's combat node calls; hooked so a server NPC fights only whom the server says.
  void* request_combat = nullptr;
  // The defence's listener on its body's combat events (defence, source, attack, data,
  // event), counted for the fight log.
  void* defense_event = nullptr;
};

// The NPC's combat automation, switched on when it is off; while its director idles it is asked
// to enter the fight. For the log: on (+64), the director's state (+132; 0 idle), the opponent
// its combat core caches ([combat + 528] + 3320) and the director's last answer.
struct Automation {
  bool on = false;
  std::int32_t director = -1;
  void* opponent = nullptr;
  int entered = -1;
  int attacking = -1;
  // The automation context's mode ([automation + 72] + 204; 4 lets an attack start) and its
  // + 192 (3 refuses one).
  int mode = -1;
  int context_192 = -1;
  int armed = -1;
  bool drawn = false;
  // This call drew the weapon set the body carries.
  bool drew = false;
  bool weapons_found = false;
  std::uint64_t weapon_ids[6] = {};
  bool busy = false;
  void* busy_table = nullptr;
  void* busy_check = nullptr;
  int context = -1;
  std::int32_t opponent_state_1 = -1;
  std::int32_t opponent_state_0 = -1;
};
// With draw_once the body first draws the weapon set it carries (once per fight); the melee
// offense starts on a later call, with the weapon in hand.
Automation enable_automation(void* npc_actor, bool draw_once);

// Swings the NPC's weapon (or fists) at its opponent in an attack zone, as its combat automation
// attacks; false when the game made no attack (no combat actor, automation or opponent).
bool swing(void* npc_actor, std::int32_t zone);

// The share of the attacks coming at the NPC's body (its actor) it parries (perfect block), 0 to
// 1; below 0 (the default) leaves every defence to the game's own combat brain.
void set_defense_chance(void* npc_actor, float chance);

// Defences the game asked for since the last call (every body's) and those let through.
struct DefenseCounts {
  unsigned events = 0;   // combat events at the bodies' defences
  unsigned attacks = 0;  // of those, attacks coming at the body
  unsigned checked = 0;  // of those, attacks the game would defend against
  unsigned asked = 0;
  unsigned let_through = 0;
  // The kind the game chose last (-1 none since the last call).
  int game_kind = -1;
};
DefenseCounts take_defense_counts();

// The NPC's body draws the weapon set it carries (a second call puts it away again, live), as
// human:DrawWeapon() does; false when the body has no weapon slots.
bool draw_carried(void* npc_actor);

// Whether a defence of the NPC's body is under way (its automation context + 264).
bool defending(void* npc_actor);

// The NPC body's combat state (its core + 56: 1 idle, 2 guard, 4 ready to strike, 8 striking,
// 64 hit, 128 preparing to parry, 256 parry, 512 dodge) and its block (core + 1584, read as the
// block action's switch does); -1 when it has no combat core.
struct CombatSnapshot {
  std::int32_t state = -1;
  int block = -1;
};
CombatSnapshot combat_snapshot(void* npc_actor);

// Whether the NPC's combat automation is between attacks (a swing can start).
bool ready_to_swing(void* npc_actor);

// Gives the NPC's combat actor (actor + 632) the target's combat actor as its opponent and target,
// once the game made it; false while the NPC has none yet or the target has none.
bool face_opponent(void* npc_actor, void* target_actor);

// The NPC's own combat request (its C_NPCCombatHelper at C_NPC + 3864, slot 5): the
// state machine starts a fight against the entity of WUID `target` (the request the brain's
// combat node makes). False when a request is already pending.
bool request_combat(void* agent, std::uint64_t target);

// Ends the NPC's own combat request (its combat helper's abort, slot 6), as the
// game ends one: the helper leaves its fight and reports it ended.
void end_fight(void* agent);

// The server governs the NPC agent's fights from now on: its brain's own combat requests are
// refused unless they name the entity the server let it fight (a bandit's brain attacked a
// player on its faction's account, and again after the revive). govern() lets it fight no one
// until permit_fight(); forget_agent() when the body goes. Any thread.
// A request against whoever struck the NPC (struck_by: the striker's soul WUID) within
// kStruckFightMs passes too: refused, the struck NPC's blows were priced at nothing and the server
// never heard of them, so it could not fight back.
inline constexpr unsigned long long kStruckFightMs = 15000;
void govern(void* agent, std::uint32_t npc);
// Whether a HitInfo (its type at + 24) is a blow that may be fought back: a collision (3) is not;
// a push (10, no damage) is, except within 5 s of the local player's revive (a body
// getting up pushes whoever stands there: the revived player's pushed the bandit, which took them
// on again). The game's guards answer a player who keeps shoving them.
bool fight_back_hit(const std::uint8_t* hit);
void struck_by(std::uint32_t npc, std::uint64_t striker);
// A player (their soul's WUID) went down: no NPC fights them back for their earlier blows, and a
// fight an NPC's brain took against them ends, unless it is the server's own attack order (the
// server stops that, targetDown). Live: after the revive the bandit took them on again at once.
// Engine thread.
void forgive(std::uint64_t striker);
void permit_fight(void* agent, std::uint64_t target);
void forget_agent(void* agent);
// The brain's combat requests refused since the last call.
unsigned take_refused_requests();
// DIAG: the brain's requests let through since the last call (and the last one's target),
// and the fights forgive() ended.
struct RequestStats {
  unsigned allowed = 0;
  std::uint64_t last_target = 0;
  unsigned forgiven = 0;
};
RequestStats take_request_stats();

// Whether the game's WUID registry finds an entity for the id.
bool wuid_known(std::uint64_t wuid);

// The NPC agent's activation flags (C_NPC + 6008: bit 8 activated, bit 2 AI on; + 6009), read
// before and after activating it and switching its AI on when they were not.
struct Activation {
  std::uint8_t before = 0;
  std::uint8_t after = 0;
  std::uint8_t active = 0;
};
Activation activate_brain(void* agent);
// Npc.brain false: the agent's AI switched off as the game switches it
// (with off): the agent leaves the AI system's update and the perception, so it neither thinks nor
// notices the items and the players around it (no pick-ups, no remarks of its own).
Activation deactivate_brain(void* agent);

// False when one is missing; start and stop then do nothing.
bool install(const Natives& natives);

// Starts a skirmish of the NPC (its soul) against the target (its soul), makes the NPC (its
// engine entity id and IEntity) hostile to the target and turns its attention to it. False when
// the game refused the skirmish.
bool start(std::uint32_t npc_id, void* npc_entity, void* npc_soul, std::uint32_t target_id,
           void* target_entity, void* target_soul);

// The NPC is no longer hostile to the target.
void stop(std::uint32_t npc_id, std::uint32_t target_id);

// The game's brain fights whom its enemy check (C_RPGCombat slot 24, hooked in game_client.cpp)
// calls an enemy. An attacking NPC's soul and its target's soul are enemies of each other from
// mark() until unmark(); any thread may ask enemies().
void mark(void* npc_soul, void* target_soul);
void unmark(void* npc_soul);
bool enemies(const void* source, const void* target);

}  // namespace kcds::runtime::npcs::attack
