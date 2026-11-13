// The server's NPCs in this game. It shares the replay of a
// simulator's movement with the horses (riding/native_horses). Engine update thread only.
//
// Every client shows each NetworkNpc of its world as a body of the game's own (npcs/npc_body.hpp):
//   - the client the server chose as the NPC's simulator shows it as the soul's own class ("NPC"),
//     which runs the soul's brain and moves on its own; it reports what the body does (where it is,
//     its movement request, motion parameters and clips: NPC_STATE, the horse state's form) about
//     ten times a second;
//   - every other client shows it as "NPC_NAI", the game's NPC without AI, and replays the
//     simulator's last state on it the way a horse replica replays its simulator's (the movement
//     request, the clips per layer, the motion parameters, corrections when it drifted).
// A change of simulator makes the body anew in the class the client's new part asks for. A
// teleport (the field `teleport` went up) puts the simulator's body where the server says.
// Female NPCs ("NPC_Female") have no class without AI in the game: their replicas are "NPC_NAI"
// made a woman before its init (npc_body.hpp spawn_body).
//
// Health (npcs/npc_health.hpp): every body shows the server's `health` and takes none
// of its game's; the damage hooks report what it would have taken. When `alive` goes false every
// body dies the game's own death and lies, its movement and clips no longer captured or
// replayed. A revive raises `life`, and every client makes the body anew.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../../Backend/integration/local/runtime.hpp"
#include "kcd2s/integration/player_animation.hpp"
#include "../replicas/motion_capture.hpp"
#include "npc_walk.hpp"
#include "combat_mode.hpp"

namespace kcds::game_client {

namespace core = kcd2s::core;
using AutomationUpdate = void(__fastcall*)(void* automation);
using CreateCombatActor = void*(__fastcall*)(void* module, void* actor);
using ReleaseCombatActor = void(__fastcall*)(void* module, void** slot);
using RegisterCombatActor = void(__fastcall*)(void* list, void* combat);
using SoulFights = bool(__fastcall*)(void* soul);

// The body of a server NPC in this game: one this client simulates (authority) or a replica.
struct NativeNpc {
  // A refused walk of a new body: when it is tried again, and until when it is.
  double order_retry_at = 0.0;
  double order_retry_until = 0.0;
  std::uint32_t engine_id{};
  void* actor{};
  void* soul{};
  void* skeleton{};
  void* animated{};
  // The `life` the body was made for, and whether it was killed for the NPC's death.
  std::int64_t life{};
  bool killed{};
  // The dead body's pose: when this client saw it die, whether it sent its pose (the
  // simulator) and the reported pose it laid out last (a replica).
  double died_at = -1;
  bool pose_sent{};
  std::string pose_applied;
  // This client simulates the NPC: the body is of the soul's own class.
  bool authority{};
  // Npc.setExpression as last put on the body here.
  std::string face;
  std::uint32_t epoch{};
  std::uint64_t sequence{};
  std::uint64_t animation_sequences[16]{};
  // The teleport count the body last followed; -1 before the first.
  std::int64_t teleport = -1;
  // When the simulator last reported the body's state, and where and how it faced then.
  double reported_at{};
  double active_until{};
  core::Vec3 sent_position{};
  core::Quat sent_rotation{};
  // What the simulator's body wears as last sent (NPC_OUTFIT), and when it is read next.
  std::string sent_outfit;
  // The `animation` field the simulator's body last played (Npc.playAnimation), and the watch of
  // its clip until it is over (tick_npc_clip): the request's sequence, the clip on the body's
  // layer, whether it loops and when it started.
  std::string animation;
  bool clip_watched{};
  std::uint32_t clip_sequence{};
  std::int32_t clip_id = -1;
  bool clip_loop{};
  double clip_started{};
  // A fragment the resource plays on the body instead, watched in player_animation
  // until it ends.
  bool fragment_watched{};
  // The `intent` field the simulator's body last took (Npc.moveTo and the other orders), the
  // order parsed from it, and a follow's state: walking after the target (last seen where),
  // caught up once, and when it looks again.
  std::string intent;
  kcds::runtime::npcs::walk::Order order;
  bool following{};
  bool caught_up{};
  // Running to catch up with the followed body.
  bool follow_running{};
  // The replica's last simulator state glided to and when it arrived.
  std::uint32_t glide_sequence{};
  double glide_seen_at{};
  core::Vec3 follow_target{};
  double next_follow{};
  // When a lookAt last turned the body (0 when it has none).
  double looked_at{};
  // The engine id of the body an attack made the NPC hostile to (0 for none), and when an attack
  // that has not taken yet is tried again.
  std::uint32_t attacked_engine{};
  double next_attack{};
  // The player who last struck the NPC while it fought another, when, and whether
  // they joined its fight: for a while the NPC turns on them as the game's own does.
  core::EntityId striker{};
  double struck_at{};
  bool striker_joined{};
  // Whether the simulator's body had its brain checked and activated (activate_brain).
  bool brain_checked{};
  // Npc.brain (npc.hpp NpcBrain): false leaves the simulator's body without the game's brain.
  bool brain_on{true};
  // The body is the level's own entity of this person (NpcLevelGuid, issue 016): it is never
  // removed or made anew, only taken and let go; its brain runs on the simulator alone.
  bool level{};
  // A level body's own AI LOD state (agent + 1800) as the game had it before a replica switched
  // its brain off (2); given back when this game runs it. 255 while unknown.
  std::uint8_t level_lod = 255;
  // The movement trace (issue 016), told every kNpcTraceSeconds: on the simulator the states sent,
  // the longest gap and step between two and the AI LOD state; on a replica the states that came,
  // the longest gap and step between two.
  double trace_at{};
  // How often the game had sent this simulated body to its fake movement and it was taken back
  // (real_movement.hpp).
  unsigned real_movement_restored{};
  // The hands field last sent for this simulated body (issue 016), for its log line.
  std::string sent_hands;
  // Whether this replica holds the game's sitting stance for its seated simulator (issue 016).
  bool sitting_held{};
  // The simulator's side: when its body last held a seat, and whether it was sent as sitting.
  double seat_held_at{};
  bool sitting_sent{};
  // The props its hands hold that are no items, as measured when first seen (right, left; issue
  // 016), kept while the same mesh stays in the hand.
  std::array<std::optional<kcd2s::integration::AnimationProp>, 2> hand_entity_props;
  unsigned trace_states{};
  double trace_longest_gap{};
  float trace_longest_step{};
  double trace_last_state_at{};
  core::Vec3 trace_last_position{};
  std::uint32_t trace_last_sequence{};
  // Npc.locomotion 'kinematic' (npc.hpp NpcKinematic): the simulator's body is put along the way
  // to `glide_to` (tick_npc_glide) instead of walking it with the game's movement; `glided_at` is
  // when it last moved, 0 before the first step.
  bool kinematic{};
  bool gliding{};
  core::Vec3 glide_to{};
  double glided_at{};
  // A walk's way around a player standing on it (tick_npc_avoid): where the order's walk goes,
  // whether the body is stepping around someone now, when it looks ahead next.
  core::Vec3 walk_goal{};
  bool walk_goal_set{};
  bool detouring{};
  double next_avoid{};
  // When the order's walk began (-1: just given, taken on the next look) and the last way around
  // another body began (tick_npc_avoid).
  double walk_started_at{-1.0};
  double detoured_at{-100.0};
  // Where the body stood when the order's walk last moved it, and since when (tick_npc_stall;
  // a negative time: not looked at yet).
  core::Vec3 stall_at{};
  double stall_since{-1.0};
  // An attack's fight (tick_npc_fight): when it looks again, whether it walks to the target,
  // when it swings next and when the swing's press ends, and whether it was logged.
  double next_fight_step{};
  bool fight_walking{};
  // The game's morale_max buff on its soul while it attacks (its instance id, 0 when none).
  std::uint64_t fight_morale{};
  // Until when its step back from a target it came too close to runs (none: 0).
  double fight_back_until{};
  // Its combat state and block as last logged.
  std::int32_t logged_combat_state = -2;
  int logged_block = -2;
  double next_swing{};
  double swing_release{};
  std::int32_t swing_zone = 0;
  // The fight drew the weapon the body carries (once per attack order).
  bool weapon_drawn = false;
  // A replica holding nothing while armed looks again then (its draw may have come before the
  // outfit gave it the weapon).
  double next_draw_check{};
  // The simulator reported its body's face (NPC_LOOK).
  bool look_sent = false;
  double next_look{};
  bool fight_logged{};
  bool controller_logged{};
  // The `appearance` field the body was last built from (Npc.setAppearance).
  std::string appearance;
  double next_outfit{};
  core::Vec3 followed_position{};
  double followed_at{};
};

// The NPCs' bodies in this game, by network id.
extern std::unordered_map<core::EntityId, NativeNpc> native_npcs;

// A living server NPC with a body here, for the client resources (Follow): its network
// id, its body's engine id, where the server has it and its name.
struct NpcBody {
  core::EntityId id{};
  std::uint32_t engine_id{};
  core::Vec3 position{};
  std::string name;
};

// The living NPCs with a body here, as tick_native_npcs last saw them, by network id. Engine
// thread.
const std::vector<NpcBody>& npc_bodies();
// The actor of a server NPC's body here, null when it has none.
void* server_npc_actor(std::uint32_t npc);
// Gives the simulated body the game's morale_max until its next order other than an attack
// Engine thread.
void brace_for_fight(core::EntityId npc);

// A player's blow landed on an NPC this game simulates: if the NPC fights someone
// else, it turns on that player for a while, the player joining its fight. Any thread.
void note_player_strike(core::EntityId npc, core::EntityId player);
// The game's automation update and "does this soul fight" (hooked: the originals), and the combat
// actor natives an NPC replica gets for lock-on.
extern AutomationUpdate original_automation_update;
extern SoulFights original_soul_fights;
extern CreateCombatActor create_combat_actor;
extern ReleaseCombatActor release_combat_actor;
extern RegisterCombatActor register_combat_actor;
extern kcds::runtime::npcs::CombatModeQuery npc_combat_mode;

void clear_native_npcs(void* entity_system);
// Ends every NPC body's walk, while the bodies are still there: leaving to the main
// menu forgets the NPCs without removing their bodies (the level unload does), and a path search
// still queued for one ran on in the next level, the Animation Studio's, and crashed there.
void stop_native_npc_walks(void* entity_system);
// The engine id of an NPC's body here and its actor (0 and null when it has none).
std::uint32_t engine_body_of(core::EntityId network_id, void*& actor);
// Whether an NPC's body here is its lasting one: its simulator is known.
bool npc_body_settled(core::EntityId network_id);
void __fastcall hooked_automation_update(void* automation);
bool __fastcall hooked_soul_fights(void* soul);
// Retire departures before any network character factory runs.
template <class Entities>
void retire_native_npcs(const Entities& entities, void* entity_system);

// Every frame, with the replicated entities (instantiated for the client's mirror).
template <class Entities>
void tick_native_npcs(const Entities& entities, void* actor_system, void* entity_system,
                      const float* view, double now);

}  // namespace kcds::game_client
