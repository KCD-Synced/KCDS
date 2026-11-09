// The server's NPCs in this game (see native_npcs.hpp). Engine update thread only.
#include "native_npcs.hpp"
#include "../replicas/character_retirement.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <mutex>

#include "../animation/animation_props.hpp"
#include "../animation/player_animation.hpp"
#include "../appearance/components.hpp"
#include "../audio/voice_files.hpp"
#include "../branding/runtime_branding.hpp"
#include "../body_state/body_readings.hpp"
#include "../appearance/outfit.hpp"
#include "../buffs/player_buffs.hpp"
#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../seats/seat_collisions.hpp"
#include "../dice/dice_table.hpp"
#include "../core/natives.hpp"
#include "../crafting/sounds.hpp"
#include "../hud/speech_bubbles.hpp"
#include "../knockout/knockout.hpp"
#include "../physics/line_of_sight.hpp"
#include "../ranged/shots.hpp"
#include "../weapons/drawn_weapons.hpp"
#include "../doors/doors.hpp"
#include "../props/pose_math.hpp"
#include "../props/prop_entity.hpp"
#include "../replicas/lod_guard.hpp"
#include "../seats/seat_reservations.hpp"
#include "../replicas/removal_guard.hpp"
#include "../replicas/remote.hpp"
#include "../riding/native_horses.hpp"
#include "../riding/trample.hpp"
#include "MinHook.h"
#include "kcd2s/bridge/sigscan.hpp"
#include "kcd2s/integration/animation_props.hpp"
#include "../combat/combat_identity.hpp"
#include "../core/game_layouts.hpp"
#include "npc_attack.hpp"
#include "npc_barks.hpp"
#include "npc_body.hpp"
#include "npc_health.hpp"
#include "npc_speech.hpp"
#include "npc_suppression.hpp"
#include "real_movement.hpp"

using namespace kcd2s;

namespace kcds::game_client {

std::unordered_map<core::EntityId, NativeNpc> native_npcs;
std::unordered_map<core::EntityId, double> npc_retry_at;

// What each NPC says (Npc.say, the `speech` field), kept apart from its body so a new body of the
// same NPC goes on with it.
std::unordered_map<core::EntityId, kcds::runtime::npcs::Speech> npc_speeches;

// How often the simulator reports its NPC's state while it moves, turns or is in combat mode,
// and while it stands still; how far it moved (squared metres) or how little
// its facing still matches (quaternion dot) to count as moving.
constexpr double kNpcActiveReportSeconds = 0.05;
constexpr double kNpcIdleReportSeconds = 0.2;
constexpr double kNpcActiveTailSeconds = 0.5;
constexpr float kNpcMovedSquared = 0.03f * 0.03f;
constexpr float kNpcTurnedDot = 0.99995f;
constexpr double kNpcOutfitSeconds = 1.0;
// A follow looks at its target twice a second and walks again once the target moved a metre; a
// flee runs this many metres past its radius.
constexpr double kNpcFollowSeconds = 0.5;
// A walking NPC steps around a player standing on its way (tick_npc_avoid): it looks ahead this
// often, that far and that wide, and walks to a point that far beside the player.
constexpr double kNpcAvoidSeconds = 0.2;
constexpr float kNpcAvoidAhead = 3.0f;
constexpr float kNpcAvoidWidth = 0.9f;
constexpr float kNpcAvoidAside = 1.8f;
// When a way around may begin: a second after the walk began, 1.5 s after the last
// one, with the body facing within 45 degrees of its way.
constexpr double kNpcAvoidAfterStart = 1.0;
constexpr double kNpcDetourGap = 1.5;
constexpr float kNpcAvoidFacingCos = 0.7071f;
// A kinematic body's pace (metres a second) for the orders' walk, jog and run, the longest step
// it takes in one frame (seconds), and how far above and below its way it looks for the ground.
// How long a resource's clip blends in before its watch judges it, and the normalized time a
// one-shot ends at.
constexpr double kNpcClipGraceSeconds = 0.4;
constexpr float kNpcClipEnd = 0.999f;
constexpr float kGlideWalk = 1.5f;
constexpr float kGlideJog = 3.2f;
constexpr float kGlideRun = 5.5f;
constexpr double kGlideLongestStep = 0.1;
constexpr float kGlideGroundAbove = 1.5f;
constexpr float kGlideGroundBelow = 4.0f;
constexpr float kNpcFollowMoved = 1.0f;
// A follower this far past its radius runs to catch up (a dog behind a running owner),
// and goes at its order's pace again once it is within kNpcFollowCaughtUp of it.
constexpr float kNpcFollowCatchUp = 4.0f;
constexpr float kNpcFollowCaughtUp = 1.5f;
constexpr int kOrderRun = 3;
constexpr float kNpcFleeMargin = 3.0f;
// A lookAt turns the body this fast, and stops turning once it faces the target this closely.
constexpr float kNpcTurnRadiansPerSecond = 3.0f;
constexpr float kNpcFacedRadians = 0.05f;
// An attack whose target's body is not in this game yet is tried again this often.
constexpr double kNpcAttackRetrySeconds = 1.0;
constexpr double kNpcDrawCheckSeconds = 2.0;
// The class of a body no client simulates here.
constexpr char kNpcReplicaClass[] = "NPC_NAI";

namespace {

// The local player's lock on an NPC whose body is made again: locking on to an NPC
// another game runs hands it to this game, and its body is made anew in the class with AI, so the
// lock pointed at the body that went and dropped at once (the player's swings
// then hit nothing and the NPC never learnt who attacked it). The lock goes to the new body.
constexpr std::size_t kLocalMovement = 632;
constexpr std::size_t kMovementCore = 528;
constexpr std::size_t kCoreSelected = 3320;
constexpr std::size_t kMovementController = 1104;
constexpr ULONGLONG kRelockMs = 5000;

struct Relock {
  core::EntityId npc = 0;
  std::uint8_t hard = 0;
  ULONGLONG until = 0;
};
Relock relock;

// Bodies taken away whose NPC gets a new body once the entity system has removed them (network
// id -> the old body's engine id and when it went), at most kBodyGoingSeconds.
struct BodyGoing {
  std::uint32_t engine = 0;
  double since = 0;
};
std::map<core::EntityId, BodyGoing> bodies_going;
constexpr double kBodyGoingSeconds = 2.0;

// The bodies this client asked the entity system to remove (slot 19, not at once: the game removes
// one at the end of its frame). One still there after kRemovalCheckSeconds the game kept, and a
// server NPC made again at its place would stand beside it (the same people twice):
// it is removed at once then, and logged.
struct BodyRemoved {
  std::uint32_t engine = 0;
  core::EntityId id = 0;
  std::chrono::steady_clock::time_point since;
};
std::vector<BodyRemoved> bodies_removed;
constexpr double kRemovalCheckSeconds = 3.0;

// The local player's movement and its selected opponent (an IEntity), null when there is none.
void* local_movement() {
  auto* actor = static_cast<std::uint8_t*>(local_combat_actor.load());
  return actor ? *reinterpret_cast<void**>(actor + kLocalMovement) : nullptr;
}

void* local_selection(void* movement) {
  auto* core =
      movement
          ? *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement) + kMovementCore)
          : nullptr;
  return core ? *reinterpret_cast<void**>(core + kCoreSelected) : nullptr;
}

// The lock the game dropped on a body this long before the body goes still counts (the game lets
// go of a handed NPC's old body first: 0.16 s before).
constexpr ULONGLONG kLostLockMs = 1000;

// Before a body goes: is it the local player's lock, or the lock the game just let go?
void note_relock(core::EntityId id, void* entity) {
  void* movement = local_movement();
  if (!entity || !movement) {
    return;
  }
  const ULONGLONG now = GetTickCount64();
  std::uint8_t hard = 0;
  if (local_selection(movement) == entity) {
    auto* controller = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement) +
                                                         kMovementController);
    hard = controller ? controller[kHardLockFlag] : 0;
  } else if (lost_lock_entity.load() == entity && now - lost_lock_at.load() < kLostLockMs) {
    hard = lost_lock_hard.load();
  } else {
    return;
  }
  relock.npc = id;
  relock.hard = hard;
  relock.until = now + kRelockMs;
  log("COMBAT relock npc=%u kept while its body is made again (hard=%u)", id, unsigned(hard));
}

// After the NPC's new body is made: the lock on it again, as the player had it.
void apply_relock(core::EntityId id, void* entity) {
  if (relock.npc != id || !entity) {
    return;
  }
  const bool fresh = GetTickCount64() < relock.until;
  relock.npc = 0;
  void* movement = local_movement();
  if (!fresh || !movement || local_selection(movement)) {
    return;
  }
  hooked_set_combat_target(movement, entity);
  auto* controller =
      *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement) + kMovementController);
  if (controller) {
    controller[kHardLockFlag] = relock.hard;
  }
  log("COMBAT relock npc=%u on its new body %p (hard=%u)", id, entity, unsigned(relock.hard));
}

bool npc_combat_metrics_enabled() {
  static const bool enabled = [] {
    wchar_t hooks[4]{};
    wchar_t metrics[4]{};
    return GetEnvironmentVariableW(L"KCDS_TEST_HOOKS", hooks, 4) == 1 && hooks[0] == L'1' &&
           GetEnvironmentVariableW(L"KCDS_TEST_CHARACTER_METRICS", metrics, 4) == 1 &&
           metrics[0] == L'1';
  }();
  return enabled;
}

struct NpcCombatCounts {
  std::uint64_t observations = 0;
  std::uint64_t components = 0;
  std::uint64_t active = 0;
  std::uint64_t unknown = 0;

  void add(const kcds::runtime::npcs::CombatModeObservation& observation) {
    ++observations;
    components += observation.component_present;
    active += observation.mode == kcds::runtime::npcs::CombatMode::Active;
    unknown += observation.mode == kcds::runtime::npcs::CombatMode::Unknown;
  }
};

void report_npc_combat_metrics(double now, const NpcCombatCounts& frame) {
  struct Window {
    double started = -1;
    std::uint64_t frames = 0;
    NpcCombatCounts total;
  };
  static thread_local Window window;
  if (window.started < 0 || now < window.started) {
    window = {};
    window.started = now;
  }
  ++window.frames;
  window.total.observations += frame.observations;
  window.total.components += frame.components;
  window.total.active += frame.active;
  window.total.unknown += frame.unknown;
  if (now - window.started < 5) {
    return;
  }
  const double frames = static_cast<double>(window.frames);
  log("NPC COMBAT interval_s=%.3f frames=%llu authority_mean=%.2f component_present_mean=%.2f "
      "native_active_mean=%.2f unknown_mean=%.2f observations=%llu scope=authority_with_skeleton",
      now - window.started, static_cast<unsigned long long>(window.frames),
      window.total.observations / frames, window.total.components / frames,
      window.total.active / frames, window.total.unknown / frames,
      static_cast<unsigned long long>(window.total.observations));
  window = {};
}

}  // namespace
// The game's buff morale_max (Tables rpg/buff.xml, 61bf5b0d-aa94-45cc-9cdd-dd76d3903189).
constexpr integration::Guid kFightMoraleBuff = {0x0d, 0x5b, 0xbf, 0x61, 0x94, 0xaa, 0xcc, 0x45,
                                                0x9c, 0xdd, 0xdd, 0x76, 0xd3, 0x90, 0x31, 0x89};

// The fight's morale buff comes off when the NPC is given an order other than an attack.
void drop_fight_morale(NativeNpc& npc) {
  if (npc.fight_morale) {
    kcds::runtime::buffs::remove_instance(npc.fight_morale);
    npc.fight_morale = 0;
  }
}

// The last line each NPC simulated here said without a bubble of the game's (a voice only), by its
// text key: its bubble is shown here as on every other game (issue 016).
std::unordered_map<core::EntityId, std::string> bubbleless_lines;

// Shows what the NPC says over its body while it lasts and the camera is near. The voice of the
// NPC's own line plays on this copy of it, except on its simulator, whose game played it.
void update_npc_speech(core::EntityId id, const std::string& field, const std::string& name,
                       void* entity, const float* view, double now) {
  auto& speech = npc_speeches[id];
  const auto body = native_npcs.find(id);
  if (kcds::runtime::npcs::take_speech(speech, field, name, now)) {
    log("NPC says npc=%u: %s", id, speech.text.c_str());
    const bool simulated = body != native_npcs.end() && body->second.authority;
    // Its simulator's game shows the line as its own subtitle (issue 016).
    speech.subtitle_wanted = !simulated;
    if (!speech.voice.empty() && !simulated && entity) {
      void* component = kcds::runtime::crafting::sounds::audio_of(entity);
      const bool played =
          kcds::runtime::audio::voices::play(component, speech.voice, speech.purpose);
      log("NPC voice npc=%u played=%u: %s", id, unsigned(played), speech.voice.c_str());
    }
  }
  // The NPC's own line its simulator reported: this simulator's game shows it already, unless the
  // game said it without a bubble (a voice only): then this game shows the bubble too, as every
  // other game does (issue 016: only one player saw the bubble).
  const auto voiced = bubbleless_lines.find(id);
  const bool without_bubble = voiced != bubbleless_lines.end() && voiced->second == speech.text;
  if (speech.native && body != native_npcs.end() && body->second.authority && !without_bubble) {
    kcds::runtime::npcs::end_speech(speech);
    return;
  }
  const bool was_subtitled = speech.subtitled;
  kcds::runtime::npcs::show_speech(speech, entity, view, now);
  if (speech.subtitled && !was_subtitled) {
    log("NPC subtitle npc=%u shown: %s", id, speech.text.c_str());
  }
}

// Players' blows on NPCs this game simulates, for the tick: NPC -> player.
std::mutex player_strikes_mutex;
std::vector<std::pair<core::EntityId, core::EntityId>> player_strikes;
// How long an NPC fighting another turns on the player who struck it, seconds.
constexpr double kTurnOnStrikerSeconds = 10.0;

void note_player_strike(core::EntityId npc, core::EntityId player) {
  if (npc == 0 || player == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(player_strikes_mutex);
  if (player_strikes.size() < 64) {
    player_strikes.emplace_back(npc, player);
  }
}

// The strikes noted for this NPC since its last tick: the latest striker is kept with the time.
void take_player_strikes(core::EntityId id, NativeNpc& npc, double now) {
  std::lock_guard<std::mutex> lock(player_strikes_mutex);
  for (auto it = player_strikes.begin(); it != player_strikes.end();) {
    if (it->first != id) {
      ++it;
      continue;
    }
    if (npc.striker != it->second) {
      npc.striker_joined = false;
    }
    npc.striker = it->second;
    npc.struck_at = now;
    it = player_strikes.erase(it);
  }
}

// Whom an attacking NPC fights now: the player who struck it while it fought another, for
// kTurnOnStrikerSeconds, else its ordered target.
core::EntityId current_foe(const NativeNpc& npc, double now) {
  const bool on_striker = npc.striker != 0 && npc.striker != npc.order.target &&
                          now - npc.struck_at < kTurnOnStrikerSeconds;
  return on_striker ? npc.striker : npc.order.target;
}

// The game's morale check (crime_hasMoraleForCombat: 0.8, or 0.3 and more than the opponent's)
// sends an attacking NPC that fails it out of the fight (a bandit of morale 0.34 fought a
// few swings, then put its weapon away and left). morale_max (mor=1) on the simulated body while
// it attacks keeps it in.
void brace_for_fight(core::EntityId id) {
  const auto found = native_npcs.find(id);
  if (found == native_npcs.end() || !found->second.authority || !found->second.soul) {
    return;
  }
  NativeNpc& npc = found->second;
  if (!npc.fight_morale) {
    npc.fight_morale = kcds::runtime::buffs::add_to_soul(npc.soul, kFightMoraleBuff);
    log("NPC fight npc=%u morale buff instance=%llu", id,
        static_cast<unsigned long long>(npc.fight_morale));
  }
}

bool simulated_npc_skeleton(void* skeleton) {
  for (const auto& [id, npc] : native_npcs) {
    if (npc.skeleton == skeleton) {
      return npc.authority;
    }
  }
  return false;
}

// The NPC's body goes: out of the capture and replay of movement, and out of the world.
// Whether a body's head is low above its feet, as when it sits (issue 016): a standing body's
// head is 1.5 m up or more, a sitting one's about 1.2 m.
constexpr float kSittingHeadHeight = 1.35f;
// How long a seat counts as held after the game last showed it held (issue 016).
constexpr double kSeatGraceSeconds = 2.0;

bool head_low(void* entity, const core::Vec3& feet) {
  float head[3] = {};
  float turn[4] = {};
  if (!entity || !kcds::runtime::props::entity::joint_pose(entity, "Head", head, turn)) {
    return false;
  }
  return head[2] - feet.z < kSittingHeadHeight;
}

// A prop a hand holds that is no item (issue 016: the knife a bench's action gave a sitting NPC):
// the entity bound to the hand's attachment ("hand_right", "hand_left"; a CEntityAttachment, whose
// + 8 is the entity's id). ICharacterInstance slot 9 is its attachment manager, whose
// slot 8 gives an attachment by name (smithing.cpp); IAttachment slot 28 its bound object (+ 32).
// The entity's mesh is hung on the hand's joint at where it is now from the joint, as
// the replicas show it.
void* entity_attachment_vtable() {
  static void* const table = [] {
    using kcds::game_client::image;
    using kcds::game_client::image_size;
    if (!image) {
      return static_cast<void*>(nullptr);
    }
    const std::size_t at = kcd2s::bridge::rtti_vtable(
        image, image_size, reinterpret_cast<std::uintptr_t>(image), ".?AVCEntityAttachment@@");
    return at == kcd2s::bridge::knpos ? static_cast<void*>(nullptr)
                                      : static_cast<void*>(image + at);
  }();
  return table;
}

bool hand_entity_prop(void* entity_system, void* body, bool left,
                      integration::AnimationProp& prop) {
  using GetCharacter = void*(__fastcall*)(void*, int);
  using Get = void*(__fastcall*)(void*);
  using ByName = void*(__fastcall*)(void*, const char*);
  void* character = body ? slot<GetCharacter>(body, 101)(body, 0) : nullptr;
  void* manager = character ? slot<Get>(character, 9)(character) : nullptr;
  void* attachment =
      manager ? slot<ByName>(manager, 8)(manager, left ? "hand_left" : "hand_right") : nullptr;
  void* bound = attachment ? slot<Get>(attachment, 28)(attachment) : nullptr;
  void* table = entity_attachment_vtable();
  if (!bound || !table || *static_cast<void**>(bound) != table) {
    return false;
  }
  const std::uint32_t held_id =
      *reinterpret_cast<const std::uint32_t*>(static_cast<std::uint8_t*>(bound) + 8);
  void* held = held_id ? get_entity(entity_system, held_id) : nullptr;
  const std::string model =
      held ? kcds::runtime::props::entity::mesh_file(held, integration::kNpcHandsMax / 2)
           : std::string();
  if (model.empty()) {
    return false;
  }
  const char* joint = left ? "LeftHand" : "RightHand";
  float joint_at[3] = {};
  float joint_turn[4] = {};
  float held_at[3] = {};
  float held_turn[4] = {};
  if (!kcds::runtime::props::entity::joint_pose(body, joint, joint_at, joint_turn)) {
    return false;
  }
  kcds::runtime::props::entity::world_pose(held, held_at, held_turn);
  std::array<float, 3> offset{};
  std::array<float, 4> turn{};
  kcds::runtime::pose_math::relative(joint_at, joint_turn, held_at, held_turn, offset.data(),
                                     turn.data());
  // A prop in the hand sits at the hand. Measured while the game lets it go (a fleeing NPC drops
  // what it carries) it was far from it, and the replicas kept it there, on the head or in the air
  // such a prop is not held.
  constexpr float kMostFromHand = 0.6f;
  const float away =
      std::sqrt(offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2]);
  if (away > kMostFromHand) {
    log("NPC hand prop %s %.2f m from the %s hand: not held", model.c_str(), away,
        left ? "left" : "right");
    return false;
  }
  prop.model = model;
  prop.left_hand = left;
  prop.joint = joint;
  prop.position = offset;
  prop.rotation = turn;
  return true;
}

// Tells an NPC whose part of the tick took longer than kSlowNpcMs, with the stage it ended in.
constexpr double kSlowNpcMs = 300.0;

// A step of the NPC tick (what, and the NPC for one NPC's part) that took longer than kSlowNpcMs.
struct SlowNpcWatch {
  SlowNpcWatch(const char* step, std::uint32_t npc_id)
      : what(step), npc(npc_id), started(std::chrono::steady_clock::now()) {
  }
  ~SlowNpcWatch() {
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    if (ms > kSlowNpcMs) {
      log("NPC slow %s npc=%u %.0f ms, ended in %s", what, npc, ms,
          engine_stage ? engine_stage : "?");
    }
  }
  const char* what;
  std::uint32_t npc;
  std::chrono::steady_clock::time_point started;
};

// What a simulated body holds in its hands, as an animation props field (issue 016): each hand's
// item class on that hand's slot, where the game holds it ("" for empty hands). Its replicas hang
// the same items there, and the game's clips the replica plays move the slots as they move them
// here.
std::string npc_hands_field(void* entity_system, void* body, NativeNpc& npc) {
  std::vector<integration::AnimationProp> props;
  for (const bool left : {false, true}) {
    std::array<std::uint8_t, 16> item_class{};
    integration::AnimationProp prop;
    if (npc.actor && kcds::runtime::weapons::class_in_hand(npc.actor, left, item_class)) {
      prop.item = item_class;
      prop.left_hand = left;
      prop.joint = left ? "LeftHandSlot" : "RightHandSlot";
      props.push_back(prop);
      continue;
    }
    // A prop that is no item: measured where it sits on the hand once, and kept while the hand
    // holds the same mesh, so the field does not change (and the replicas' prop is not hung anew)
    // every time the hand moves.
    auto& kept = npc.hand_entity_props[left ? 1 : 0];
    if (!hand_entity_prop(entity_system, body, left, prop)) {
      kept.reset();
      continue;
    }
    if (!kept || kept->model != prop.model) {
      kept = prop;
    }
    props.push_back(*kept);
  }
  return integration::animation_props_field(props);
}

void remove_npc_body(void* entity_system, core::EntityId id, NativeNpc& npc) {
  // The items its hands showed go with it (issue 016).
  kcds::runtime::animation_props::forget(id);
  kcds::runtime::player_animation::forget_replica_talking(id);
  {
    std::lock_guard<std::shared_mutex> lock(motion_mutex);
    horse_captures.erase(npc.skeleton);
    remote_inputs.erase(npc.skeleton);
    animation_stop_fences.erase(npc.skeleton);
  }
  kcds::runtime::npcs::health::forget(id);
  void* entity = entity_system ? get_entity(entity_system, npc.engine_id) : nullptr;
  if (entity) {
    // The resource's fragment ends while the body is still there to take the stop.
    kcds::runtime::player_animation::stop_on_body(id);
  } else {
    kcds::runtime::player_animation::forget_body(id);
  }
  // The level's own person: its entity stays the level's; it is only let go.
  if (npc.level) {
    if (entity) {
      kcds::runtime::npcs::walk::stop(entity);
    }
    log("NPC level body let go network=%u engine=0x%x", id, npc.engine_id);
    return;
  }
  if (entity) {
    note_relock(id, entity);
    // Its walk ends first: a path search still running for the body would write into what goes
    // with it (the planner through; live: the game crashed moments after a
    // player left walking NPCs behind).
    kcds::runtime::npcs::walk::stop(entity);
    // An attacking NPC's fight ends first: its combat request is aborted and it leaves the
    // enemy pair, so nothing of the fight points at the body that goes (removing an
    // attacking NPC crashed its target's game).
    if (npc.attacked_engine) {
      kcds::runtime::npcs::attack::end_fight(kcds::runtime::npcs::walk::agent_of(entity));
      kcds::runtime::npcs::attack::unmark(npc.soul);
      npc.attacked_engine = 0;
    }
    kcds::runtime::npcs::attack::forget_agent(kcds::runtime::npcs::walk::agent_of(entity));
    drop_fight_morale(npc);
    // A body in a fight goes as a player's replica does: no combat animation is
    // queued into it while it goes.
    record_character_retirement(entity_system, npc.engine_id);
    kcds::runtime::replicas::begin_replica_removal(npc.actor, unsigned(id));
    slot<void(__fastcall*)(void*, std::uint32_t, bool)>(entity_system, 19)(entity_system,
                                                                           npc.engine_id, false);
    kcds::runtime::replicas::end_replica_removal();
    bodies_removed.push_back(BodyRemoved{npc.engine_id, id, std::chrono::steady_clock::now()});
  }
  // The outfit kept for the body (its key is the NPC's id, above every player's).
  kcds::runtime::appearance::outfit::forget(id);
  log("NPC body removed network=%u engine=0x%x", id, npc.engine_id);
  kcds::runtime::seats::collisions::forget(id);
}

void stop_native_npc_walks(void* entity_system) {
  if (!entity_system) {
    return;
  }
  unsigned stopped = 0;
  for (auto& [id, npc] : native_npcs) {
    (void)id;
    if (void* entity = get_entity(entity_system, npc.engine_id)) {
      kcds::runtime::npcs::walk::stop(entity);
      ++stopped;
    }
  }
  log("NPC walks stopped before leaving the world: %u bodies", stopped);
}

void clear_native_npcs(void* entity_system) {
  for (auto& [id, npc] : native_npcs) {
    remove_npc_body(entity_system, id, npc);
  }
  native_npcs.clear();
  kcds::runtime::npcs::health::clear();
  npc_retry_at.clear();
  bodies_going.clear();
  bodies_removed.clear();
  for (auto& [id, speech] : npc_speeches) {
    kcds::runtime::npcs::end_speech(speech);
  }
  npc_speeches.clear();
}

// A follow of the simulator's position, as a horse replica follows its simulator's.
void follow_replicated_npc(NativeNpc& npc, void* entity, const core::Vec3& p, const core::Quat& q,
                           const core::Vec3* velocity, double now) {
  NativeHorse follower;
  follower.followed_position = npc.followed_position;
  follower.followed_at = npc.followed_at;
  follow_replicated_horse(follower, entity, p, q, velocity, now);
  npc.followed_position = follower.followed_position;
  npc.followed_at = follower.followed_at;
}

// A fighting NPC's replica glides to the simulator's position and facing every frame (its 20 Hz
// state, not the slower world snapshot): the walk follower above lets it drift a metre and turn
// 20 degrees before it corrects, so in a fight it swung from the wrong place and facing and then
// jumped (the NPC's attack lagged for the other players).
constexpr float kFightFollowRate = 18.0f;
constexpr float kFightSnapMeters = 1.5f;

void follow_fighting_npc(NativeNpc& npc, void* entity, const core::Vec3& p, const core::Quat& q,
                         double now, float snap_meters = kFightSnapMeters,
                         float follow_rate = kFightFollowRate) {
  const float* tm = slot<const float*(__fastcall*)(void*)>(entity, 32)(entity);
  const double elapsed = npc.followed_at > 0 ? now - npc.followed_at : 0;
  npc.followed_at = now;
  if (!tm) {
    return;
  }
  const float dx = p.x - tm[3];
  const float dy = p.y - tm[7];
  const float dz = p.z - tm[11];
  core::Vec3 position = p;
  core::Quat facing = q;
  if (dx * dx + dy * dy + dz * dz < snap_meters * snap_meters) {
    // The share of the way covered this frame, independent of the frame rate.
    const float share = 1.0f - std::exp(-follow_rate * float(std::min(elapsed, 0.1)));
    position = core::Vec3{tm[3] + dx * share, tm[7] + dy * share, tm[11] + dz * share};
    const float from = matrix_yaw(tm);
    const float turn = std::remainder(rotation_yaw(q) - from, 6.2831853f);
    const float yaw = from + turn * share;
    facing = core::Quat{0, 0, std::sin(yaw * 0.5f), std::cos(yaw * 0.5f)};
  }
  set_transform(entity, position, facing);
  npc.followed_position = position;
}

// An animal's replica (a dog that trots or runs) follows the same way, to the
// simulator's 20 Hz state a little ahead along its velocity: the walk follower above let its
// native movement stall and put it back on the slower world snapshot every frame, so it moved in
// steps. It jumps only when it is kAnimalSnapMeters away (a teleport, a long network gap).
constexpr float kAnimalSnapMeters = 4.0f;
constexpr float kAnimalFollowRate = 12.0f;
// How far ahead along its velocity a state is taken, at most (seconds since it arrived).
constexpr double kAnimalLeadSeconds = 0.15;

void follow_animal_npc(NativeNpc& npc, void* entity, const integration::NpcState& state,
                       double now) {
  // The height the game left the body at this frame, before this follow sets it (the trace below).
  const float* before = slot<const float*(__fastcall*)(void*)>(entity, 32)(entity);
  const float native_z = before ? before[11] : 0.0f;
  if (state.sequence != npc.glide_sequence) {
    npc.glide_sequence = state.sequence;
    npc.glide_seen_at = now;
  }
  core::Vec3 target = state.position;
  if (state.motion_valid) {
    const float lead = float(std::min(now - npc.glide_seen_at, kAnimalLeadSeconds));
    target.x += state.velocity.x * lead;
    target.y += state.velocity.y * lead;
  }
  follow_fighting_npc(npc, entity, target, state.rotation, now, kAnimalSnapMeters,
                      kAnimalFollowRate);
  // Test hook: every frame's shown and newest place of the replica, to measure smoothness.
  static const bool trace = GetEnvironmentVariableW(L"KCDS_TEST_REPLICA_TRACE", nullptr, 0) > 0;
  if (trace) {
    log("SMOOTH npc now=%.4f shown=%.3f,%.3f,%.3f newest=%.3f,%.3f,%.3f native=%.3f", now,
        npc.followed_position.x, npc.followed_position.y, npc.followed_position.z, state.position.x,
        state.position.y, state.position.z, native_z);
  }
}

// Plays the resource's clip ("<sequence>|<loop>|<clip>", npc.hpp NpcAnimation) on the
// simulator's body, or stops it for "". Replicas replay the clip with the simulator's others.
// Tells the server the clip a resource played on an NPC simulated here is over
// (NPC_ANIMATION_END), so the resource hears npcAnimationEnd.
void send_npc_animation_end(core::EntityId id, const NativeNpc& npc, std::uint8_t reason) {
  bool sent = false;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    sent = client->peer().send_npc_animation_end(id, npc.epoch, npc.clip_sequence, reason);
  }
  log("NPC animation end npc=%u request=%u reason=%u sent=%u", id, npc.clip_sequence,
      unsigned(reason), unsigned(sent));
}

// The NPC_ANIMATION_END reason of how a fragment's action ended.
std::uint8_t npc_end_reason(integration::AnimationReason reason) {
  if (reason == integration::AnimationReason::Finished) {
    return integration::kNpcAnimationFinished;
  }
  if (reason == integration::AnimationReason::Refused) {
    return integration::kNpcAnimationRefused;
  }
  return integration::kNpcAnimationInterrupted;
}

// Plays the resource's clip ("<sequence>|<loop>|<clip>", "" stops it) or fragment
// ("<sequence>|<loop>||<fragment>|<tags>") on the simulator's body and watches it until
// it is over; a clip or fragment the body does not have is reported refused at once. Watchers see
// a fragment as any other animation of the body: its clips go out with the simulator's others.
void apply_npc_animation(core::EntityId id, NativeNpc& npc, void* entity,
                         const std::string& animation) {
  kcds::runtime::player_animation::stop_on_body(id);
  npc.fragment_watched = false;
  kcds::runtime::npcs::FragmentField request;
  if (kcds::runtime::npcs::fragment_field_of(animation, request)) {
    npc.clip_watched = false;
    kcds::runtime::npcs::apply_animation_field(entity, std::string());
    npc.clip_sequence = request.sequence;
    void* controller = kcds::runtime::body_state::action_controller(npc.actor);
    const bool queued = kcds::runtime::player_animation::play_on_body(
        id, controller, request.fragment, request.tags, request.loop, request.sequence);
    log("NPC animation npc=%u fragment %s tags '%s' loop=%u %s", id, request.fragment.c_str(),
        request.tags.c_str(), unsigned(request.loop),
        queued ? "queued" : "refused (no such fragment)");
    if (!queued) {
      send_npc_animation_end(id, npc, integration::kNpcAnimationRefused);
      return;
    }
    npc.fragment_watched = true;
    return;
  }
  const bool played = kcds::runtime::npcs::apply_animation_field(entity, animation);
  log("NPC animation npc=%u %s %s", id, animation.empty() ? "stop" : animation.c_str(),
      played ? "plays" : "not in the body's animations");
  npc.clip_watched = false;
  if (animation.empty()) {
    return;
  }
  const std::size_t bar = animation.find('|');
  npc.clip_sequence = std::uint32_t(std::strtoul(animation.c_str(), nullptr, 10));
  npc.clip_loop = bar != std::string::npos && animation.compare(bar + 1, 1, "1") == 0;
  if (!played) {
    send_npc_animation_end(id, npc, integration::kNpcAnimationRefused);
    return;
  }
  float time = 0;
  npc.clip_id = -1;
  kcds::runtime::npcs::layer_clip(entity, npc.clip_id, time);
  npc.clip_watched = true;
}

// The watch of the resource's clip on the simulator's body: a one-shot that reached its last key
// is finished; a clip that left the body's layer, or gave way to another, was interrupted (a hit,
// a fall). The first moments of a clip are its blend-in and are not judged.
void tick_npc_clip(core::EntityId id, NativeNpc& npc, void* entity, double now) {
  if (npc.fragment_watched) {
    void* controller = kcds::runtime::body_state::action_controller(npc.actor);
    const auto reason = kcds::runtime::player_animation::watch_body(id, controller);
    if (reason) {
      npc.fragment_watched = false;
      log("NPC animation npc=%u fragment ended: %s", id,
          kcds::runtime::player_animation::end_detail());
      send_npc_animation_end(id, npc, npc_end_reason(*reason));
    }
  }
  if (!npc.clip_watched) {
    return;
  }
  if (npc.clip_started <= 0) {
    npc.clip_started = now;
  }
  if (now - npc.clip_started < kNpcClipGraceSeconds) {
    return;
  }
  std::int32_t clip = -1;
  float time = 0;
  const bool there = kcds::runtime::npcs::layer_clip(entity, clip, time);
  if (!there || (npc.clip_id >= 0 && clip != npc.clip_id)) {
    npc.clip_watched = false;
    send_npc_animation_end(id, npc, integration::kNpcAnimationInterrupted);
    return;
  }
  if (npc.clip_id < 0) {
    npc.clip_id = clip;
  }
  if (!npc.clip_loop && time >= kNpcClipEnd) {
    npc.clip_watched = false;
    send_npc_animation_end(id, npc, integration::kNpcAnimationFinished);
  }
}

// Tells the server an order of an NPC simulated here ended (npc.hpp NPC_INTENT_DONE), so the
// resource hears npcIntentDone and a patrol steps on.
void send_npc_intent_done(core::EntityId id, std::uint32_t epoch, std::uint32_t sequence,
                          std::uint8_t status) {
  bool sent = false;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    sent = client->peer().send_npc_intent_done(id, epoch, sequence, status);
  }
  log("NPC order done npc=%u order=%u status=%u sent=%u", id, sequence, unsigned(status),
      unsigned(sent));
}

// Sends the simulator's body from `from` to `destination` for its order: walked by the game's
// own movement, or for a kinematic NPC put along the way frame by frame (tick_npc_glide). False
// when the game refused the walk.
bool move_body(core::EntityId id, NativeNpc& npc, void* entity, const float from[3],
               const float destination[3], int speed) {
  namespace walk = kcds::runtime::npcs::walk;
  if (!npc.kinematic) {
    npc.gliding = false;
    return walk::walk_to(entity, id, npc.order.sequence, from, destination, speed);
  }
  walk::stop(entity);
  npc.gliding = true;
  npc.glide_to = core::Vec3{destination[0], destination[1], destination[2]};
  npc.glided_at = 0;
  return true;
}

// Whether `other` is the body `own` or one of its class (IEntity GetClass, slot 3).
bool same_kind(void* other, void* own) {
  if (other == own) {
    return true;
  }
  using GetClass = void*(__fastcall*)(void*);
  return slot<GetClass>(other, 3)(other) == slot<GetClass>(own, 3)(own);
}

// A kinematic body's step along its way this frame: straight on at its order's pace, standing on
// whatever ground is under it and facing where it goes. At the end a moveTo or flee reports that
// it was reached; a follow's next look decides what comes after.
void tick_npc_glide(core::EntityId id, NativeNpc& npc, void* entity, double now) {
  if (!npc.gliding) {
    return;
  }
  const double elapsed = npc.glided_at > 0 ? std::min(now - npc.glided_at, kGlideLongestStep) : 0;
  npc.glided_at = now;
  const float* tm = slot<const float*(__fastcall*)(void*)>(entity, 32)(entity);
  const float dx = npc.glide_to.x - tm[3];
  const float dy = npc.glide_to.y - tm[7];
  const float distance = std::sqrt(dx * dx + dy * dy);
  const float pace = npc.order.speed == 3   ? kGlideRun
                     : npc.order.speed == 2 ? kGlideJog
                                            : kGlideWalk;
  const float step = pace * float(elapsed);
  // The ground is looked for from the body's own height, the last step too: the order's height
  // (the resource's point) can lie under the ground there, and the body then fell through.
  core::Vec3 next{npc.glide_to.x, npc.glide_to.y, tm[11]};
  const bool arrived = distance <= step;
  if (!arrived && distance > 0) {
    next.x = tm[3] + dx / distance * step;
    next.y = tm[7] + dy / distance * step;
  }
  const float above[3] = {next.x, next.y, next.z + kGlideGroundAbove};
  const float below[3] = {next.x, next.y, next.z - kGlideGroundBelow};
  float down = 0;
  // Not on itself nor on a body of its kind: a small body (a hen) lies under the ray's start, and
  // standing on its own collider or on the hens beside it, it climbed into the sky.
  if (kcds::runtime::physics::first_hit_passing(above, below, &same_kind, entity, down)) {
    next.z = above[2] - down;
  }
  // The body faces its way: a turn about the vertical axis from the game's forward, +y.
  core::Quat facing{};
  if (distance > 0.01f) {
    const float yaw = std::atan2(dy, dx) - 1.5707963f;
    facing = core::Quat{0, 0, std::sin(yaw / 2), std::cos(yaw / 2)};
  } else {
    facing = npc.sent_rotation;
  }
  set_transform(entity, next, facing);
  if (!arrived) {
    return;
  }
  npc.gliding = false;
  if (npc.order.kind == "follow") {
    npc.following = false;
    return;
  }
  log("NPC glide npc=%u reached (%.2f %.2f %.2f)", id, next.x, next.y, next.z);
  send_npc_intent_done(id, npc.epoch, npc.order.sequence, integration::kNpcIntentReached);
}

// How long after its body appears a refused walk is tried again, and how often: a
// body just made has no working AI yet, and the game refuses its walk.
constexpr double kOrderRetrySeconds = 6.0;
constexpr double kOrderRetryEvery = 0.5;

// Carries out the resource's order on the simulator's body (npc.hpp NpcIntent): a moveTo walks
// it there on the game's navmesh, any other order stops its walk. The orders the body does not
// carry out yet (patrol, follow, flee, lookAt, attack) are logged.
void apply_npc_intent(core::EntityId id, NativeNpc& npc, void* entity, const std::string& intent) {
  namespace walk = kcds::runtime::npcs::walk;
  if (npc.attacked_engine) {
    kcds::runtime::npcs::attack::stop(npc.engine_id, npc.attacked_engine);
    kcds::runtime::npcs::attack::end_fight(kcds::runtime::npcs::walk::agent_of(entity));
    kcds::runtime::npcs::attack::unmark(npc.soul);
    kcds::runtime::npcs::attack::permit_fight(kcds::runtime::npcs::walk::agent_of(entity), 0);
    log("NPC attack npc=%u ends", id);
    npc.attacked_engine = 0;
  }
  npc.next_attack = 0;
  npc.weapon_drawn = false;
  npc.gliding = false;
  npc.walk_goal_set = false;
  npc.detouring = false;
  npc.order = walk::Order{};
  npc.following = false;
  npc.caught_up = false;
  npc.next_follow = 0;
  const bool parsed = !intent.empty() && walk::parse_order(intent, npc.order);
  if (!parsed || npc.order.kind != "attack") {
    drop_fight_morale(npc);
  }
  if (!parsed) {
    npc.order = walk::Order{};
    walk::stop(entity);
    log("NPC order npc=%u '%s': stops", id, intent.c_str());
    return;
  }
  const walk::Order& order = npc.order;
  const float* tm = slot<const float*(__fastcall*)(void*)>(entity, 32)(entity);
  const float from[3] = {tm[3], tm[7], tm[11]};
  if (order.kind == "follow") {
    // The follow walks in tick_npc_follow, as its target moves.
    log("NPC order npc=%u follows %u within %.1f m", id, order.target, order.radius);
    return;
  }
  if (order.kind == "attack") {
    // The fight starts in tick_npc_attack, once the target's body is here.
    walk::stop(entity);
    log("NPC order npc=%u attacks %u", id, order.target);
    return;
  }
  if (order.kind == "lookAt") {
    // The body stands and turns in tick_npc_look; looking has no end to report.
    walk::stop(entity);
    log("NPC order npc=%u looks at %u (%.1f %.1f)", id, order.target, order.destination[0],
        order.destination[1]);
    return;
  }
  float destination[3] = {order.destination[0], order.destination[1], order.destination[2]};
  if (order.kind == "flee") {
    // Away from the point, straight on, to a little past `radius` from it; the game's path finds
    // the way there.
    float dx = from[0] - order.destination[0];
    float dy = from[1] - order.destination[1];
    float length = std::sqrt(dx * dx + dy * dy);
    if (length < 0.1f) {
      dx = 1;
      dy = 0;
      length = 1;
    }
    const float run = order.radius + kNpcFleeMargin;
    destination[0] = order.destination[0] + dx / length * run;
    destination[1] = order.destination[1] + dy / length * run;
    destination[2] = from[2];
  } else if (order.kind != "moveTo") {
    walk::stop(entity);
    log("NPC order npc=%u %s not carried out by the body yet", id, order.kind.c_str());
    return;
  }
  npc.walk_goal = core::Vec3{destination[0], destination[1], destination[2]};
  npc.walk_goal_set = true;
  npc.detouring = false;
  npc.walk_started_at = -1.0;
  const bool started = move_body(id, npc, entity, from, destination, order.speed);
  const char* how = npc.kinematic ? "glides" : "walks";
  log("NPC order npc=%u %s to (%.2f %.2f %.2f) speed=%d %s", id, order.kind.c_str(), destination[0],
      destination[1], destination[2], order.speed, started ? how : "refused by the game");
  if (!started) {
    const double now = local::clock_seconds();
    if (now < npc.order_retry_until) {
      npc.order_retry_at = now + kOrderRetryEvery;
      return;
    }
    send_npc_intent_done(id, npc.epoch, order.sequence, integration::kNpcIntentFailed);
  }
}

// Where a player or an NPC is, as this client last heard from the server; false when unknown.
template <class Entities>
bool network_position(const Entities& entities, core::EntityId id, core::Vec3& position) {
  const auto found = entities.find(id);
  if (found == entities.end()) {
    return false;
  }
  const auto& e = found->second;
  if (e.type == core::type_id("NetworkPlayer")) {
    position = std::get<core::Vec3>(e.fields[local::Position]);
    return true;
  }
  if (e.type == core::type_id("NetworkNpc") && e.fields.size() > integration::NpcPosition) {
    position = std::get<core::Vec3>(e.fields[integration::NpcPosition]);
    return true;
  }
  return false;
}

// The engine id and actor of a player's or an NPC's body in this game: the local player's own,
// another player's replica, or an NPC's body; 0 when it has none here.
std::uint32_t engine_body_of(core::EntityId network_id, void*& actor) {
  actor = nullptr;
  if (network_id == local_network_id.load()) {
    actor = local_combat_actor.load();
    return local_engine_id.load();
  }
  if (const auto remote = remotes.find(network_id); remote != remotes.end()) {
    actor = remote->second.actor;
    return remote->second.engine_id;
  }
  if (const auto npc = native_npcs.find(network_id); npc != native_npcs.end()) {
    actor = npc->second.actor;
    return npc->second.engine_id;
  }
  return 0;
}

// Whether the NPC has its lasting body here. Until the server names its simulator the body is a
// replica's, and the game of the player who then simulates it makes it anew.
bool npc_body_settled(core::EntityId network_id) {
  const auto npc = native_npcs.find(network_id);
  return npc != native_npcs.end() && (npc->second.authority || npc->second.epoch != 0);
}

// Diagnostic: how often the game updates the watched NPC's combat automation.
AutomationUpdate original_automation_update = nullptr;
std::atomic<void*> watched_automation{nullptr};
std::atomic<unsigned> automation_updates{0};

void __fastcall hooked_automation_update(void* automation) {
  if (automation && automation == watched_automation.load()) {
    automation_updates.fetch_add(1);
  }
  original_automation_update(automation);
}

// The fight of an attacking NPC, once the game gave it its combat actor (its own combat brain
// does not move a runtime body, live): out of reach it walks to the target on the navmesh;
// within reach it faces it and presses attack through its combat controller's input, as a
// player's controller does, so the game's own combat picks and lands the swing.
constexpr float kNpcFightReach = 2.2f;
// Closer than this the NPC steps back to about kNpcFightSpacing before it swings again: its
// combat moves step in on every swing, and inside a metre neither side's blows land (an
// NPC that crept to 0.1 m of the player, both swinging at air).
constexpr float kNpcFightTooClose = 1.1f;
constexpr float kNpcFightSpacing = 1.7f;
constexpr double kNpcFightBackSeconds = 1.2;
constexpr double kNpcFightStepSeconds = 0.4;
constexpr double kNpcFightSwingSeconds = 1.6;
constexpr double kNpcFightPressSeconds = 0.12;
// A bow attack: the archer keeps between kNpcBowNear and kNpcBowFar of its target
// (walking to kNpcBowKeep when out of that), faces it and shoots every kNpcBowShotSeconds with
// the speed a player's bow gives an arrow (40 to 47 live), aimed above the target's chest by the
// arrow's drop over the distance. The arrow is the game's normal arrow (arrow_normal).
constexpr float kNpcBowNear = 4.0f;
constexpr float kNpcBowFar = 35.0f;
constexpr float kNpcBowKeep = 20.0f;
constexpr double kNpcBowShotSeconds = 3.0;
constexpr float kNpcBowArrowSpeed = 45.0f;
constexpr float kNpcBowGravity = 9.81f;
constexpr float kNpcBowLaunchHeight = 1.45f;
constexpr float kNpcBowLaunchForward = 0.6f;
constexpr float kNpcBowAimHeight = 1.3f;
constexpr char kNpcBowArrow[] = "ad6f0f01-aec4-44d1-982c-1210eb01b74a";

// The simulator's archer shoots at its target: a shot of the game's own projectile from its
// body (ranged::replay, as another player's shot from their replica), and NPC_SHOT, which the
// server relays so every other client fires the same arrow from the NPC's body there and the
// game of the player it hits works the hit out.
void npc_bow_shot(core::EntityId id, NativeNpc& npc, void* entity, const float from[3],
                  const core::Vec3& target, float distance) {
  kcd2s::integration::Shot shot;
  integration::Guid arrow{};
  if (!integration::parse_guid(kNpcBowArrow, arrow)) {
    return;
  }
  std::memcpy(shot.ammo.data(), arrow.data(), shot.ammo.size());
  const float dx = target.x - from[0];
  const float dy = target.y - from[1];
  const float forward_x = distance > 0.05f ? dx / distance : 0.0f;
  const float forward_y = distance > 0.05f ? dy / distance : 1.0f;
  shot.position[0] = from[0] + forward_x * kNpcBowLaunchForward;
  shot.position[1] = from[1] + forward_y * kNpcBowLaunchForward;
  shot.position[2] = from[2] + kNpcBowLaunchHeight;
  // Aimed above the chest by the drop over the flight's time.
  const float flight = distance / kNpcBowArrowSpeed;
  const float drop = 0.5f * kNpcBowGravity * flight * flight;
  float aim[3] = {target.x - shot.position[0], target.y - shot.position[1],
                  target.z + kNpcBowAimHeight + drop - shot.position[2]};
  const float length = std::sqrt(aim[0] * aim[0] + aim[1] * aim[1] + aim[2] * aim[2]);
  if (!(length > 0.1f)) {
    return;
  }
  for (int axis = 0; axis < 3; ++axis) {
    shot.direction[axis] = aim[axis] / length;
  }
  shot.speed = kNpcBowArrowSpeed;
  shot.kind = 0;
  const bool fired = kcds::runtime::ranged::replay(npc.actor, entity, shot);
  bool sent = false;
  if (fired) {
    std::lock_guard<std::mutex> lock(network_mutex);
    sent = client->peer().send_npc_shot(id, shot);
  }
  log("NPC bow npc=%u shoots at %u (%.1f m): fired=%u sent=%u", id, npc.order.target, distance,
      unsigned(fired), unsigned(sent));
}

// A bow attack's step on the simulator's body: closer or back to its range, else it
// stands, faces its target and shoots on its cadence once its weapon is drawn.
template <class Entities>
void tick_npc_bow(const Entities& entities, core::EntityId id, NativeNpc& npc, void* entity,
                  double now) {
  namespace walk = kcds::runtime::npcs::walk;
  if (now < npc.next_fight_step) {
    return;
  }
  npc.next_fight_step = now + kNpcFightStepSeconds;
  take_player_strikes(id, npc, now);
  const core::EntityId foe = current_foe(npc, now);
  core::Vec3 target{};
  if (!network_position(entities, foe, target)) {
    return;
  }
  const float* tm = slot<const float*(__fastcall*)(void*)>(entity, 32)(entity);
  const float from[3] = {tm[3], tm[7], tm[11]};
  const float dx = target.x - from[0];
  const float dy = target.y - from[1];
  const float distance = std::sqrt(dx * dx + dy * dy);
  if (distance > kNpcBowFar || distance < kNpcBowNear) {
    // To kNpcBowKeep from the target along the line between them.
    const float mx = target.x - npc.follow_target.x;
    const float my = target.y - npc.follow_target.y;
    if (!npc.fight_walking || mx * mx + my * my > 1.0f) {
      const float along_x = distance > 0.05f ? dx / distance : 0.0f;
      const float along_y = distance > 0.05f ? dy / distance : 1.0f;
      const float to[3] = {target.x - along_x * kNpcBowKeep, target.y - along_y * kNpcBowKeep,
                           target.z};
      npc.fight_walking = walk::walk_to(entity, id, npc.order.sequence, from, to, 3);
      npc.follow_target = target;
    }
    return;
  }
  if (npc.fight_walking) {
    walk::stop(entity);
    npc.fight_walking = false;
  }
  const float facing = std::atan2(-dx, dy);
  const core::Quat q{0, 0, std::sin(facing * 0.5f), std::cos(facing * 0.5f)};
  set_transform(entity, core::Vec3{from[0], from[1], from[2]}, q);
  if (!npc.weapon_drawn || now < npc.next_swing) {
    return;
  }
  npc.next_swing = now + kNpcBowShotSeconds;
  npc_bow_shot(id, npc, entity, from, target, distance);
}

template <class Entities>
void tick_npc_fight(const Entities& entities, core::EntityId id, NativeNpc& npc, void* entity,
                    double now) {
  namespace walk = kcds::runtime::npcs::walk;
  if (npc.order.kind != "attack" || !npc.attacked_engine || !npc.actor) {
    return;
  }
  if (npc.order.bow) {
    tick_npc_bow(entities, id, npc, entity, now);
    return;
  }
  namespace attack = kcds::runtime::npcs::attack;
  // Its combat state and block as they change, every frame (when does it block).
  const auto combat = attack::combat_snapshot(npc.actor);
  if (combat.state != npc.logged_combat_state || combat.block != npc.logged_block) {
    npc.logged_combat_state = combat.state;
    npc.logged_block = combat.block;
    log("NPC combat npc=%u state=%d block=%d", id, combat.state, combat.block);
  }
  if (now < npc.next_fight_step) {
    return;
  }
  npc.next_fight_step = now + kNpcFightStepSeconds;
  take_player_strikes(id, npc, now);
  const core::EntityId foe = current_foe(npc, now);
  core::Vec3 target{};
  if (!network_position(entities, foe, target)) {
    return;
  }
  const float* tm = slot<const float*(__fastcall*)(void*)>(entity, 32)(entity);
  const float from[3] = {tm[3], tm[7], tm[11]};
  const float dx = target.x - from[0];
  const float dy = target.y - from[1];
  const float distance = std::sqrt(dx * dx + dy * dy);
  if (distance > kNpcFightReach) {
    // A new walk only when not walking yet or the target moved on a metre (a walk started
    // anew every step never gets going).
    const float mx = target.x - npc.follow_target.x;
    const float my = target.y - npc.follow_target.y;
    if (!npc.fight_walking || mx * mx + my * my > 1.0f) {
      const float keep = (kNpcFightReach * 0.6f) / distance;
      const float to[3] = {target.x - dx * keep, target.y - dy * keep, target.z};
      npc.fight_walking = walk::walk_to(entity, id, npc.order.sequence, from, to, 3);
      npc.follow_target = target;
    }
    return;
  }
  if (distance < kNpcFightTooClose) {
    // Back away from the target along the line between them (straight back when on top of it),
    // a new step only once the last had its time.
    if (now >= npc.fight_back_until) {
      const float back_x = distance > 0.05f ? -dx / distance : 0.0f;
      const float back_y = distance > 0.05f ? -dy / distance : -1.0f;
      const float to[3] = {target.x + back_x * kNpcFightSpacing,
                           target.y + back_y * kNpcFightSpacing, target.z};
      walk::walk_to(entity, id, npc.order.sequence, from, to, 3);
      npc.fight_walking = true;
      npc.fight_back_until = now + kNpcFightBackSeconds;
    }
    return;
  }
  npc.fight_back_until = 0;
  if (npc.fight_walking) {
    walk::stop(entity);
    npc.fight_walking = false;
  }
  // Face the target, then swing.
  const float facing = std::atan2(-dx, dy);
  const core::Quat q{0, 0, std::sin(facing * 0.5f), std::cos(facing * 0.5f)};
  set_transform(entity, core::Vec3{from[0], from[1], from[2]}, q);
  // Swings wait for the weapon in hand (the attack's first step draws it).
  // No swing while its defence is under way (a swing kept it from blocking, live).
  if (!npc.weapon_drawn || now < npc.next_swing || attack::defending(npc.actor) ||
      !attack::ready_to_swing(npc.actor)) {
    return;
  }
  // The next of the four attack zones each swing.
  const std::int32_t zone = npc.swing_zone;
  npc.swing_zone = (npc.swing_zone + 1) % 4;
  const bool swung = attack::swing(npc.actor, zone);
  npc.next_swing = now + kNpcFightSwingSeconds;
  log("NPC fight npc=%u swings at %u zone=%d (%.1f m): %s", id, foe, zone, distance,
      swung ? "attack made" : "no attack");
}

// An attack on the simulator's body: once the target's body is in this game, the NPC is made
// hostile to it and turns its attention to it (npc_attack.hpp); its own brain fights from there.
// Tried again every second until it took.
void tick_npc_attack(void* entity_system, core::EntityId id, NativeNpc& npc, void* entity,
                     double now) {
  if (npc.order.kind == "attack" && npc.attacked_engine && now >= npc.next_attack) {
    // While it attacks: the NPC's combat helper (C_NPC + 3864) as its state machine ticks it,
    // logged every two seconds (pending +144, phase +16, result +20, state change +176) and the
    // NPC's update reasons (+6010).
    npc.next_attack = now + 2;
    // Once the game gave the NPC its combat actor, the target is its opponent and target (its
    // own automation never picks one for a runtime body, live). Struck by another player, it
    // turns on them for a while, as the game's own NPCs do (it kept fighting the
    // ordered target while another player's sword took its health).
    take_player_strikes(id, npc, now);
    const core::EntityId foe = current_foe(npc, now);
    const bool on_striker = foe != npc.order.target;
    void* target_actor = nullptr;
    const std::uint32_t foe_engine = engine_body_of(foe, target_actor);
    if (on_striker && !npc.striker_joined && target_actor && npc.soul) {
      // The striker joins the NPC's fight: hostile, its skirmish, its attention.
      void* foe_entity = foe_engine ? get_entity(entity_system, foe_engine) : nullptr;
      void* foe_soul = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(target_actor) + 1640);
      if (foe_entity && foe_soul) {
        kcds::runtime::npcs::attack::mark(npc.soul, foe_soul);
        const bool joined = kcds::runtime::npcs::attack::start(npc.engine_id, entity, npc.soul,
                                                               foe_engine, foe_entity, foe_soul);
        log("NPC fight npc=%u turns on player=%u who struck it (skirmish=%u)", id, foe,
            unsigned(joined));
      }
      npc.striker_joined = true;
    }
    // The game lets the NPC fight only the foe it is permitted (its swings at another were
    // refused, live): the permit follows the current foe, and back to the ordered target.
    if (target_actor) {
      if (void* foe_soul =
              *reinterpret_cast<void**>(static_cast<std::uint8_t*>(target_actor) + 1640)) {
        kcds::runtime::npcs::attack::permit_fight(
            kcds::runtime::npcs::walk::agent_of(entity),
            *reinterpret_cast<const std::uint64_t*>(static_cast<std::uint8_t*>(foe_soul) + 48));
      }
    }
    const bool faced = kcds::runtime::npcs::attack::face_opponent(npc.actor, target_actor);
    // With its opponent set, its combat automation fights it: the first step draws the weapon
    // the body carries, the later ones fight with it.
    const auto automation =
        kcds::runtime::npcs::attack::enable_automation(npc.actor, !npc.weapon_drawn);
    if (automation.drew) {
      npc.weapon_drawn = true;
    }
    {
      auto* combat = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(npc.actor) + 632);
      watched_automation.store(combat ? *reinterpret_cast<void**>(combat + 584) : nullptr);
      log("NPC attack npc=%u automation updates in 2 s: %u", id, automation_updates.exchange(0));
    }
    log("NPC attack npc=%u opponent set=%u automation on=%u director=%d cached opponent=%p "
        "offense handle=%d feature=%d mode=%d ctx244=%d busy=%u context=%d opponent states=%d/%d",
        id, unsigned(faced), unsigned(automation.on), automation.director, automation.opponent,
        automation.entered, automation.attacking, automation.mode, automation.context_192,
        unsigned(automation.busy), automation.context, automation.opponent_state_1,
        automation.opponent_state_0);
    const auto defenses = kcds::runtime::npcs::attack::take_defense_counts();
    auto* combat_actor =
        *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(npc.actor) + 632);
    auto* automation_object =
        combat_actor ? *reinterpret_cast<std::uint8_t**>(combat_actor + 584) : nullptr;
    auto* context =
        automation_object ? *reinterpret_cast<std::uint8_t**>(automation_object + 72) : nullptr;
    const void* pending = context ? *reinterpret_cast<void**>(context + 264) : nullptr;
    log("NPC attack npc=%u weapon drawn=%u defences asked=%u let through=%u game kind=%d "
        "pending=%p events=%u attacks=%u checked=%u",
        id, unsigned(npc.weapon_drawn), defenses.asked, defenses.let_through, defenses.game_kind,
        pending, defenses.events, defenses.attacks, defenses.checked);
    if (automation.busy_table) {
      const auto base = reinterpret_cast<std::uintptr_t>(image);
      log("NPC attack npc=%u busy object vtable IDA=0x%llx check IDA=0x%llx", id,
          static_cast<unsigned long long>(
              0x180000000ULL + reinterpret_cast<std::uintptr_t>(automation.busy_table) - base),
          static_cast<unsigned long long>(
              0x180000000ULL + reinterpret_cast<std::uintptr_t>(automation.busy_check) - base));
    }
    auto* agent = static_cast<std::uint8_t*>(kcds::runtime::npcs::walk::agent_of(entity));
    if (agent) {
      const std::uint8_t* helper = agent + 3864;
      // The body's combat actor (actor + 632, give_npc_combat_actor below).
      void* combat = npc.actor
                         ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(npc.actor) + 632)
                         : nullptr;
      log("NPC attack npc=%u helper pending=%u phase=%u result=%d change=%d end=%d reasons=0x%x "
          "combat actor=%p",
          id, unsigned(helper[144]), unsigned(helper[16]),
          *reinterpret_cast<const std::int32_t*>(helper + 20),
          *reinterpret_cast<const std::int32_t*>(helper + 176),
          *reinterpret_cast<const std::int32_t*>(helper + 32), unsigned(agent[6010]), combat);
    }
    return;
  }
  if (npc.order.kind != "attack" || npc.attacked_engine || now < npc.next_attack) {
    return;
  }
  npc.next_attack = now + kNpcAttackRetrySeconds;
  void* target_actor = nullptr;
  const std::uint32_t target = engine_body_of(npc.order.target, target_actor);
  void* target_entity = target ? get_entity(entity_system, target) : nullptr;
  if (!target_entity || !target_actor || !npc.soul) {
    return;
  }
  // The game's enemy check calls the two enemies (its brain fights an enemy it perceives), and
  // the target is its attention target.
  void* target_soul = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(target_actor) + 1640);
  kcds::runtime::npcs::attack::mark(npc.soul, target_soul);
  const bool skirmish = kcds::runtime::npcs::attack::start(npc.engine_id, entity, npc.soul, target,
                                                           target_entity, target_soul);
  npc.attacked_engine = target;
  log("NPC attack npc=%u skirmish=%u", id, unsigned(skirmish));
  brace_for_fight(id);
  // Whether the game runs this NPC's brain: the AI LOD state of its agent (C_NPC + 1800; 2 is
  // off).
  void* agent = kcds::runtime::npcs::walk::agent_of(entity);
  const unsigned lod = agent ? *(static_cast<std::uint8_t*>(agent) + 1800) : 255u;
  // The NPC's own combat request against the target, by the target soul's world id.
  const auto wuid =
      *reinterpret_cast<const std::uint64_t*>(static_cast<std::uint8_t*>(target_soul) + 48);
  const bool known = kcds::runtime::npcs::attack::wuid_known(wuid);
  kcds::runtime::npcs::attack::permit_fight(agent, wuid);
  const bool requested = kcds::runtime::npcs::attack::request_combat(agent, wuid);
  log("NPC attack npc=%u combat request wuid=0x%llx known=%u requested=%u", id,
      static_cast<unsigned long long>(wuid), unsigned(known), unsigned(requested));
  log("NPC attack npc=%u target=%u engine=0x%x soul=%p started (lod=%u)", id, npc.order.target,
      target, target_soul, lod);
}

// A lookAt on the simulator's body: it turns to face its target (a player, an NPC or the point)
// at kNpcTurnRadiansPerSecond, about the vertical only, and keeps facing it as the target moves.
template <class Entities>
void tick_npc_look(const Entities& entities, NativeNpc& npc, void* entity, double now) {
  if (npc.order.kind != "lookAt") {
    npc.looked_at = 0;
    return;
  }
  const double elapsed = npc.looked_at > 0 ? std::min(now - npc.looked_at, 0.1) : 0.0;
  npc.looked_at = now;
  core::Vec3 target{npc.order.destination[0], npc.order.destination[1], npc.order.destination[2]};
  if (npc.order.target && !network_position(entities, npc.order.target, target)) {
    return;
  }
  const float* tm = slot<const float*(__fastcall*)(void*)>(entity, 32)(entity);
  const core::Vec3 here{tm[3], tm[7], tm[11]};
  const float dx = target.x - here.x;
  const float dy = target.y - here.y;
  if (dx * dx + dy * dy < 0.04f) {
    return;
  }
  // The body faces its +y: its yaw is the angle of that axis (column 1 of the matrix).
  const float yaw = std::atan2(-tm[1], tm[5]);
  const float wanted = std::atan2(-dx, dy);
  float turn = std::remainder(wanted - yaw, 6.2831853f);
  if (std::fabs(turn) < kNpcFacedRadians) {
    return;
  }
  const float step = kNpcTurnRadiansPerSecond * float(elapsed);
  turn = std::clamp(turn, -step, step);
  const float facing = yaw + turn;
  const core::Quat q{0, 0, std::sin(facing * 0.5f), std::cos(facing * 0.5f)};
  set_transform(entity, here, q);
}

// A walk of a moveTo or flee (a patrol's legs too) around a player or another server NPC on its
// way (the game's movement does not see those bodies here and walked the NPC through them).
// Five times a second: one within kNpcAvoidAhead in front of the body
// and kNpcAvoidWidth of its line sends it to a point beside it; the detour's end walks it on to
// the order's goal
// (report_finished_walks).
template <class Entities>
void tick_npc_avoid(const Entities& entities, core::EntityId id, NativeNpc& npc, void* entity,
                    double now) {
  namespace walk = kcds::runtime::npcs::walk;
  const bool walking = npc.order.kind == "moveTo" || npc.order.kind == "flee";
  if (!walking || !npc.walk_goal_set || npc.kinematic || npc.detouring || now < npc.next_avoid) {
    return;
  }
  npc.next_avoid = now + kNpcAvoidSeconds;
  if (npc.walk_started_at < 0) {
    npc.walk_started_at = now;
  }
  // Not while the body still turns onto its way: a new walk given then (the game's
  // task replaced) kept it turning on the spot, a full turn or more, until that walk ended. So
  // none in a walk's first second, none a second time within kNpcDetourGap, none while the body
  // faces away from its way.
  if (now - npc.walk_started_at < kNpcAvoidAfterStart || now - npc.detoured_at < kNpcDetourGap) {
    return;
  }
  const float* tm = slot<const float*(__fastcall*)(void*)>(entity, 32)(entity);
  const float here[3] = {tm[3], tm[7], tm[11]};
  const float gx = npc.walk_goal.x - here[0];
  const float gy = npc.walk_goal.y - here[1];
  const float left = std::sqrt(gx * gx + gy * gy);
  if (left < 1.0f) {
    return;
  }
  const float ux = gx / left;
  const float uy = gy / left;
  // The body's forward (its +y) on the ground, and none while it faces away from its way.
  const float fx = tm[1];
  const float fy = tm[5];
  const float forward = std::sqrt(fx * fx + fy * fy);
  if (forward <= 0.0f || (fx * ux + fy * uy) / forward < kNpcAvoidFacingCos) {
    return;
  }
  // Players, and the other server NPCs (the game's movement walked NPCs through each
  // other's bodies as well).
  const auto player_type = core::type_id("NetworkPlayer");
  const auto npc_type = core::type_id("NetworkNpc");
  for (const auto& [player, e] : entities) {
    core::Vec3 at{};
    if (e.type == player_type) {
      at = std::get<core::Vec3>(e.fields[local::Position]);
    } else if (e.type == npc_type && player != id && e.fields.size() > integration::NpcPosition) {
      at = std::get<core::Vec3>(e.fields[integration::NpcPosition]);
    } else {
      continue;
    }
    const float px = at.x - here[0];
    const float py = at.y - here[1];
    const float ahead = px * ux + py * uy;
    const float aside = px * uy - py * ux;
    if (ahead < 0.3f || ahead > kNpcAvoidAhead || ahead > left ||
        std::fabs(aside) > kNpcAvoidWidth || std::fabs(at.z - here[2]) > 2.0f) {
      continue;
    }
    // Beside the player, on the side the body already leans to: from 3 m back the body passes
    // it 1.5 m away.
    const float side = aside >= 0 ? -1.0f : 1.0f;
    const float around[3] = {at.x + uy * side * kNpcAvoidAside, at.y - ux * side * kNpcAvoidAside,
                             here[2]};
    if (walk::walk_to(entity, id, npc.order.sequence, here, around, npc.order.speed)) {
      npc.detouring = true;
      npc.detoured_at = now;
      log("NPC walk npc=%u steps around %s=%u (%.1f m ahead)", id,
          e.type == npc_type ? "npc" : "player", player, ahead);
    }
    return;
  }
}

// A follow on the simulator's body: twice a second, while the target is farther than `radius`,
// a walk to just inside it (again when the target moved on); within it the body stops, and the
// first catch-up is reported as reached.
template <class Entities>
void tick_npc_follow(const Entities& entities, core::EntityId id, NativeNpc& npc, void* entity,
                     double now) {
  namespace walk = kcds::runtime::npcs::walk;
  if (npc.order.kind != "follow" || now < npc.next_follow) {
    return;
  }
  npc.next_follow = now + kNpcFollowSeconds;
  core::Vec3 target{};
  if (!network_position(entities, npc.order.target, target)) {
    return;
  }
  const float* tm = slot<const float*(__fastcall*)(void*)>(entity, 32)(entity);
  const float from[3] = {tm[3], tm[7], tm[11]};
  const float dx = target.x - from[0];
  const float dy = target.y - from[1];
  const float distance = std::sqrt(dx * dx + dy * dy);
  if (distance <= npc.order.radius) {
    if (npc.following) {
      walk::stop(entity);
      npc.following = false;
    }
    npc.follow_running = false;
    if (!npc.caught_up) {
      npc.caught_up = true;
      send_npc_intent_done(id, npc.epoch, npc.order.sequence, integration::kNpcIntentReached);
    }
    return;
  }
  const float mx = target.x - npc.follow_target.x;
  const float my = target.y - npc.follow_target.y;
  const bool was_running = npc.follow_running;
  if (distance > npc.order.radius + kNpcFollowCatchUp) {
    npc.follow_running = true;
  } else if (distance < npc.order.radius + kNpcFollowCaughtUp) {
    npc.follow_running = false;
  }
  if (npc.following && npc.follow_running == was_running &&
      mx * mx + my * my < kNpcFollowMoved * kNpcFollowMoved) {
    return;
  }
  const float keep = npc.order.radius * 0.5f / distance;
  const float destination[3] = {target.x - dx * keep, target.y - dy * keep, target.z};
  const int speed = npc.follow_running ? kOrderRun : npc.order.speed;
  npc.following = move_body(id, npc, entity, from, destination, speed);
  npc.follow_target = target;
}

// Builds the body from the resource's character components (npc.hpp NpcAppearance).
void apply_npc_appearance(core::EntityId id, void* actor, const std::string& appearance) {
  const bool built = kcds::runtime::npcs::apply_appearance_field(actor, appearance);
  log("NPC appearance npc=%u %s %s", id, appearance.c_str(), built ? "built" : "refused");
}

// The game's combat actor of an actor (actor + 632), as it gives one to an NPC entering a fight
// made by the combat module's factory, set up through
// its vtable slots 90 (true when usable), 91 and 103, and registered with the actor
// (actor + 208); a factory-made one that is not usable is released.
CreateCombatActor create_combat_actor = nullptr;
ReleaseCombatActor release_combat_actor = nullptr;
RegisterCombatActor register_combat_actor = nullptr;
kcds::runtime::npcs::CombatModeQuery npc_combat_mode = nullptr;
constexpr std::size_t kActorCombat = kcds::layouts::Actor::kCombat;
constexpr std::size_t kActorCombatList = 208;
constexpr std::size_t kCombatUsable = 720 / 8;
constexpr std::size_t kCombatStart = 728 / 8;
constexpr std::size_t kCombatReady = 824 / 8;

// The game asks whether a soul fights at all (WHGame 1.5.5: a player's, or one whose
// brain runs) before it offers its body as a lock-on target (through the skirmish
// manager's slot 16). A replica of a server NPC runs no brain, so the answer is yes for it here:
// its fight runs on its simulator's client.
SoulFights original_soul_fights = nullptr;

bool __fastcall hooked_soul_fights(void* soul) {
  const auto body = kcds::runtime::npcs::health::body_of_soul(soul);
  // Npc.targetable false: the game never offers the body as a lock-on target.
  if (body && !kcds::runtime::npcs::health::targetable(body->npc)) {
    return false;
  }
  if (body && !body->simulated) {
    return true;
  }
  return original_soul_fights(soul);
}

// A replica of an NPC gets a combat actor, as a player's replica always has one: the game's
// lock-on takes only a body with one, so another player can lock on to an NPC whose
// fight runs on a third client.
void give_npc_combat_actor(core::EntityId id, void* actor) {
  if (!actor || !create_combat_actor || !release_combat_actor || !register_combat_actor) {
    return;
  }
  auto* combat_slot = reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + kActorCombat);
  void* module = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(get_services()) + 248);
  if (!kcds::runtime::npcs::health::targetable(id) || *combat_slot || !module) {
    return;
  }
  void* combat = create_combat_actor(module, actor);
  *combat_slot = combat;
  if (!combat) {
    return;
  }
  if (!slot<bool(__fastcall*)(void*)>(combat, kCombatUsable)(combat)) {
    release_combat_actor(module, combat_slot);
    *combat_slot = nullptr;
    log("NPC combat actor npc=%u not usable", id);
    return;
  }
  slot<void(__fastcall*)(void*)>(combat, kCombatStart)(combat);
  slot<void(__fastcall*)(void*)>(combat, kCombatReady)(combat);
  register_combat_actor(static_cast<std::uint8_t*>(actor) + kActorCombatList, combat);
  log("NPC combat actor npc=%u given to the replica", id);
}

// Makes the body of an NPC in the class this client's part asks for.
bool make_npc_body(void* actor_system, core::EntityId id, const core::Vec3& p, const core::Quat& q,
                   const std::string& soul, const char* entity_class, bool authority, bool female,
                   NativeNpc& npc) {
  char name[64];
  sprintf_s(name, "kcds_npc_%u", id);
  const float position[3] = {p.x, p.y, p.z};
  const float rotation[4] = {q.x, q.y, q.z, q.w};
  engine_stage = "NPC body";
  void* entity =
      kcds::runtime::npcs::spawn_body(name, entity_class, soul, position, rotation, female);
  if (!entity) {
    log("NPC body refused network=%u class=%s soul=%s", id, entity_class, soul.c_str());
    return false;
  }
  npc = NativeNpc{};
  npc.engine_id = *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(entity) + 12);
  record_character_created(npc.engine_id, entity);
  npc.authority = authority;
  npc.order_retry_until = local::clock_seconds() + kOrderRetrySeconds;
  // IActorSystem slot 3: GetActor(entity id).
  npc.actor =
      slot<void*(__fastcall*)(void*, std::uint32_t)>(actor_system, 3)(actor_system, npc.engine_id);
  void* character = slot<void*(__fastcall*)(void*, unsigned)>(entity, 101)(entity, 0);
  npc.skeleton = character ? slot<void*(__fastcall*)(void*)>(character, 5)(character) : nullptr;
  npc.animated = npc.actor ? slot<void*(__fastcall*)(void*)>(npc.actor, 90)(npc.actor) : nullptr;
  // The actor's soul (actor + 1640), whose health the body shows.
  npc.soul =
      npc.actor ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(npc.actor) + 1640) : nullptr;
  kcds::runtime::npcs::health::Body tracked;
  tracked.npc = id;
  tracked.simulated = authority;
  kcds::runtime::npcs::health::track(npc.engine_id, npc.soul, tracked);
  // A replica without AI (a person's) gets its combat actor here; an animal's has its own AI.
  if (!authority && std::strcmp(entity_class, kNpcReplicaClass) == 0) {
    give_npc_combat_actor(id, npc.actor);
  }
  log("NPC body network=%u engine=0x%x class=%s soul=%s actor=%p skeleton=%p", id, npc.engine_id,
      entity_class, soul.c_str(), npc.actor, npc.skeleton);
  apply_relock(id, entity);
  return true;
}

// The game's AI LOD state of an NPC's agent (C_NPC + 1800): 0 is full detail, the brain thinks and
// the body walks with the game's movement; 1 and 2 (LOD, MonsterLOD) move the body with
// C_FakeMovementManager, in jumps. The trace reports it.
constexpr std::size_t kAgentLodState = 1800;
// How often the movement trace tells each NPC's numbers (issue 016).
constexpr double kNpcTraceSeconds = 5.0;

// The simulator's trace of one NPC's sent states (issue 016), told every kNpcTraceSeconds.
void trace_sent_state(core::EntityId id, NativeNpc& npc, const core::Vec3& here, void* entity,
                      double now) {
  if (npc.trace_last_state_at > 0) {
    const double gap = now - npc.trace_last_state_at;
    const float dx = here.x - npc.trace_last_position.x;
    const float dy = here.y - npc.trace_last_position.y;
    const float step = std::sqrt(dx * dx + dy * dy);
    npc.trace_longest_gap = std::max(npc.trace_longest_gap, gap);
    npc.trace_longest_step = std::max(npc.trace_longest_step, step);
    // A body that walks moves at most about 1.5 m/s: a step this long in one report is a jump.
    constexpr float kJumpMeters = 0.8f;
    if (step > kJumpMeters) {
      void* agent = entity ? kcds::runtime::npcs::walk::agent_of(entity) : nullptr;
      log("NPC trace sim jump npc=%u step=%.2fm gap=%.3fs lod=%u order=%s from=%.2f,%.2f "
          "to=%.2f,%.2f",
          id, step, gap,
          agent ? unsigned(*(static_cast<std::uint8_t*>(agent) + kAgentLodState)) : 255u,
          npc.order.kind.c_str(), npc.trace_last_position.x, npc.trace_last_position.y, here.x,
          here.y);
    }
  }
  npc.trace_last_state_at = now;
  npc.trace_last_position = here;
  ++npc.trace_states;
  if (npc.trace_at == 0) {
    npc.trace_at = now;
  }
  if (now - npc.trace_at < kNpcTraceSeconds) {
    return;
  }
  void* agent = entity ? kcds::runtime::npcs::walk::agent_of(entity) : nullptr;
  const unsigned lod =
      agent ? unsigned(*(static_cast<std::uint8_t*>(agent) + kAgentLodState)) : 255u;
  log("NPC trace sim npc=%u sent=%u in %.1fs longest gap=%.3fs longest step=%.2fm lod=%u "
      "at=%.1f,%.1f",
      id, npc.trace_states, now - npc.trace_at, npc.trace_longest_gap, npc.trace_longest_step, lod,
      here.x, here.y);
  npc.trace_at = now;
  npc.trace_states = 0;
  npc.trace_longest_gap = 0;
  npc.trace_longest_step = 0;
}

// A replica's trace of the states that came for one NPC (issue 016), told every
// kNpcTraceSeconds: how many, the longest gap between two new ones and the longest step.
void trace_received_state(core::EntityId id, NativeNpc& npc, const integration::NpcState& state,
                          void* entity, double now) {
  if (state.sequence == npc.trace_last_sequence) {
    return;
  }
  if (npc.trace_last_state_at > 0) {
    const double gap = now - npc.trace_last_state_at;
    const float dx = state.position.x - npc.trace_last_position.x;
    const float dy = state.position.y - npc.trace_last_position.y;
    npc.trace_longest_gap = std::max(npc.trace_longest_gap, gap);
    npc.trace_longest_step = std::max(npc.trace_longest_step, std::sqrt(dx * dx + dy * dy));
  }
  npc.trace_last_sequence = state.sequence;
  npc.trace_last_state_at = now;
  npc.trace_last_position = state.position;
  ++npc.trace_states;
  if (npc.trace_at == 0) {
    npc.trace_at = now;
  }
  if (now - npc.trace_at < kNpcTraceSeconds) {
    return;
  }
  const float* tm = entity ? slot<const float*(__fastcall*)(void*)>(entity, 32)(entity) : nullptr;
  log("NPC trace replica npc=%u states=%u in %.1fs longest gap=%.3fs longest step=%.2fm "
      "sequence=%u at=%.1f,%.1f shown=%.1f,%.1f",
      id, npc.trace_states, now - npc.trace_at, npc.trace_longest_gap, npc.trace_longest_step,
      state.sequence, state.position.x, state.position.y, tm ? tm[3] : 0.0f, tm ? tm[7] : 0.0f);
  npc.trace_at = now;
  npc.trace_states = 0;
  npc.trace_longest_gap = 0;
  npc.trace_longest_step = 0;
}

// Takes the level's own entity of a person (NpcLevelGuid, issue 016) as the NPC's body: nothing is
// spawned. The simulator's keeps its brain; every other game's is driven as a replica.
bool bind_level_body(void* entity_system, void* actor_system, core::EntityId id,
                     std::uint32_t engine_id, bool authority, NativeNpc& npc) {
  void* entity = entity_system ? get_entity(entity_system, engine_id) : nullptr;
  if (!entity) {
    return false;
  }
  npc = NativeNpc{};
  npc.level = true;
  npc.engine_id = engine_id;
  npc.authority = authority;
  npc.order_retry_until = local::clock_seconds() + kOrderRetrySeconds;
  // IActorSystem slot 3: GetActor(entity id).
  npc.actor =
      slot<void*(__fastcall*)(void*, std::uint32_t)>(actor_system, 3)(actor_system, engine_id);
  void* character = slot<void*(__fastcall*)(void*, unsigned)>(entity, 101)(entity, 0);
  npc.skeleton = character ? slot<void*(__fastcall*)(void*)>(character, 5)(character) : nullptr;
  npc.animated = npc.actor ? slot<void*(__fastcall*)(void*)>(npc.actor, 90)(npc.actor) : nullptr;
  npc.soul =
      npc.actor ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(npc.actor) + 1640) : nullptr;
  kcds::runtime::npcs::health::Body tracked;
  tracked.npc = id;
  tracked.simulated = authority;
  kcds::runtime::npcs::health::track(engine_id, npc.soul, tracked);
  if (void* agent = kcds::runtime::npcs::walk::agent_of(entity)) {
    npc.level_lod = *(static_cast<std::uint8_t*>(agent) + 1800);
  }
  if (!authority) {
    give_npc_combat_actor(id, npc.actor);
  }
  log("NPC level body network=%u engine=0x%x actor=%p skeleton=%p lod=%u local=%u", id, engine_id,
      npc.actor, npc.skeleton, unsigned(npc.level_lod), unsigned(authority));
  return true;
}

// The local player's last blow on each NPC it does not simulate, and when it was sent: the blow
// starts a fight, and the server then gives the NPC to this client (npc.hpp kNpcFightSeconds).
// The body made for that knows nothing of the blow, so it is played on it again (no health: the
// server took that), and the NPC's AI turns on the player at once.
std::unordered_map<core::EntityId, std::pair<integration::NpcBlow, double>> own_npc_blows;
constexpr double kOwnBlowReplaySeconds = 3;

// The HitInfos of blows played here, kept while the game's rules may still read them.
std::array<std::array<std::uint8_t, integration::kNpcBlowHitBytes>, 16> npc_blow_hits{};
std::size_t next_npc_blow_hit = 0;

// The game's own lines over the server NPCs this client simulates (npc_barks.hpp): the bubble's
// first place over the head of one of them (within kBarkReach across, kBarkLow to kBarkHigh above
// its feet) goes to the server, which shows it to every player (NPC_BARK). The voice file the game
// plays on that NPC's audio component with the line (audio/voice_files.hpp) goes with it: a line
// waits for its voice up to kBarkVoiceWaitMs, and a voice played up to kBarkVoiceReachMs around
// the line is taken as its own.
constexpr float kBarkReach = 1.2f;
constexpr float kBarkLow = 0.8f;
constexpr float kBarkHigh = 3.2f;
constexpr std::uint64_t kBarkVoiceWaitMs = 400;
constexpr std::uint64_t kBarkVoiceReachMs = 1500;

struct PendingBark {
  integration::NpcBark bark;
  void* component = nullptr;
  std::uint64_t at = 0;
};
std::vector<PendingBark> pending_barks;
std::vector<kcds::runtime::audio::voices::Played> recent_voices;

// The server NPC this client simulates whose head the bubble is over, and its entity.
core::EntityId bark_speaker(void* entity_system, const float* position, void*& speaker_entity) {
  for (const auto& [id, npc] : native_npcs) {
    void* entity =
        npc.authority && entity_system ? get_entity(entity_system, npc.engine_id) : nullptr;
    if (!entity) {
      continue;
    }
    const float* tm = slot<const float*(__fastcall*)(void*)>(entity, 32)(entity);
    const float dx = position[0] - tm[3];
    const float dy = position[1] - tm[7];
    const float dz = position[2] - tm[11];
    if (dx * dx + dy * dy <= kBarkReach * kBarkReach && dz >= kBarkLow && dz <= kBarkHigh) {
      speaker_entity = entity;
      return id;
    }
  }
  return core::kInvalidEntity;
}

// The server NPC this client simulates whose audio component it is.
core::EntityId voice_speaker(void* entity_system, void* component) {
  if (!component || !entity_system) {
    return core::kInvalidEntity;
  }
  for (const auto& [id, npc] : native_npcs) {
    void* entity = npc.authority ? get_entity(entity_system, npc.engine_id) : nullptr;
    if (entity && kcds::runtime::crafting::sounds::audio_of(entity) == component) {
      return id;
    }
  }
  return core::kInvalidEntity;
}

// The localization key of a spoken line's file: its name after the first '_' (the file
// "Localization/dialog/.../phos2_npc__muz_samom_no_tak_jo_LquZ" says the line
// "npc__muz_samom_no_tak_jo_LquZ"); empty when the name has none.
std::string line_key_of_voice(const std::string& file) {
  const std::size_t slash = file.rfind('/');
  const std::size_t name = slash == std::string::npos ? 0 : slash + 1;
  const std::size_t underscore = file.find('_', name);
  if (underscore == std::string::npos || underscore + 1 >= file.size()) {
    return {};
  }
  return file.substr(underscore + 1);
}

// Whether the voice file is the line's own: a localized line's key ends its file's name.
bool voice_of_line(const std::string& file, const integration::NpcBark& bark) {
  return bark.localize && !bark.text.empty() && file.size() >= bark.text.size() &&
         file.compare(file.size() - bark.text.size(), bark.text.size(), bark.text) == 0;
}

// The line's voice, taken out of the recent ones: the file named after the line, whose speaker
// is the simulated NPC it played on (two people close together: the bubble's place alone may name
// the wrong one), or else a file played on the component of the NPC under the bubble near the
// line's time.
bool take_bark_voice(void* entity_system, PendingBark& pending) {
  for (auto it = recent_voices.begin(); it != recent_voices.end(); ++it) {
    if (!voice_of_line(it->file, pending.bark) || !integration::npc_voice_valid(it->file)) {
      continue;
    }
    const core::EntityId speaker = voice_speaker(entity_system, it->component);
    if (speaker != core::kInvalidEntity) {
      pending.bark.npc = static_cast<std::uint32_t>(speaker);
    }
    pending.bark.voice = it->file;
    pending.bark.purpose = it->purpose;
    recent_voices.erase(it);
    return true;
  }
  for (auto it = recent_voices.begin(); it != recent_voices.end(); ++it) {
    const std::uint64_t gap = it->at > pending.at ? it->at - pending.at : pending.at - it->at;
    if (pending.component && it->component == pending.component && gap <= kBarkVoiceReachMs &&
        integration::npc_voice_valid(it->file)) {
      pending.bark.voice = it->file;
      pending.bark.purpose = it->purpose;
      recent_voices.erase(it);
      return true;
    }
  }
  return false;
}

void send_npc_barks(void* entity_system) {
  kcds::runtime::npcs::barks::watch(kcds::runtime::hud::hud_bubbles(), log);
  const std::uint64_t now = GetTickCount64();
  for (auto& played : kcds::runtime::audio::voices::take_played()) {
    recent_voices.push_back(std::move(played));
  }
  for (const auto& placed : kcds::runtime::npcs::barks::take_placed()) {
    void* entity = nullptr;
    const core::EntityId speaker = bark_speaker(entity_system, placed.position, entity);
    if (speaker == core::kInvalidEntity) {
      continue;
    }
    PendingBark pending;
    pending.bark.npc = static_cast<std::uint32_t>(speaker);
    pending.bark.localize = placed.localize;
    pending.bark.text = placed.text;
    pending.component = kcds::runtime::crafting::sounds::audio_of(entity);
    pending.at = now;
    pending_barks.push_back(std::move(pending));
  }
  for (auto it = pending_barks.begin(); it != pending_barks.end();) {
    const bool voiced = take_bark_voice(entity_system, *it);
    if (!voiced && now - it->at < kBarkVoiceWaitMs) {
      ++it;
      continue;
    }
    const integration::NpcBark& bark = it->bark;
    bool sent = false;
    {
      std::lock_guard<std::mutex> lock(network_mutex);
      sent = client->peer().send_npc_bark(bark);
    }
    log("NPC bark npc=%u localize=%u sent=%u voice=%s purpose=%u: %s", bark.npc,
        unsigned(bark.localize), unsigned(sent), bark.voice.empty() ? "-" : bark.voice.c_str(),
        bark.purpose, bark.text.c_str());
    it = pending_barks.erase(it);
  }
  // A voice no bubble took within kBarkVoiceWaitMs, played on a server NPC this client simulates,
  // is a line the game said without a bubble: it goes alone. Others stay a while, as their line
  // may come later, and are dropped then.
  for (auto it = recent_voices.begin(); it != recent_voices.end();) {
    if (now - it->at < kBarkVoiceWaitMs) {
      ++it;
      continue;
    }
    const core::EntityId speaker = voice_speaker(entity_system, it->component);
    if (speaker != core::kInvalidEntity && integration::npc_voice_valid(it->file)) {
      // Its text, as a subtitle the game showed only to the player it spoke to: every other player
      // sees it in a bubble over the NPC.
      integration::NpcBark bark;
      bark.npc = static_cast<std::uint32_t>(speaker);
      bark.voice = it->file;
      bark.purpose = it->purpose;
      // A sound with no text (a grunt in a fight) goes as its voice alone.
      const std::string key = line_key_of_voice(it->file);
      const std::string said = key.empty() ? std::string{} : kcds::runtime::branding::localize(key);
      if (!said.empty()) {
        bark.text = key;
        bark.localize = true;
      }
      bool sent = false;
      {
        std::lock_guard<std::mutex> lock(network_mutex);
        sent = client->peer().send_npc_bark(bark);
      }
      if (sent && !bark.text.empty()) {
        bubbleless_lines[speaker] = bark.text;
      }
      log("NPC bark npc=%u without a bubble sent=%u voice=%s purpose=%u: %s (%s)", bark.npc,
          unsigned(sent), bark.voice.c_str(), bark.purpose, bark.text.c_str(), said.c_str());
      it = recent_voices.erase(it);
      continue;
    }
    if (now - it->at > kBarkVoiceReachMs) {
      log("NPC voice of no simulated NPC component=%p: %s", it->component, it->file.c_str());
      it = recent_voices.erase(it);
      continue;
    }
    ++it;
  }
}

// Sends what the damage hooks found (NPC_HIT, NPC_BLOW).
void send_npc_hits() {
  auto hits = kcds::runtime::npcs::health::take_hits();
  auto blows = kcds::runtime::npcs::health::take_blows();
  if (hits.empty() && blows.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(network_mutex);
  for (const auto& hit : hits) {
    const bool sent = client->peer().send_npc_hit(hit);
    log("NPC hit sent npc=%u attacker=%u health=%.2f reason=%u subpart=%d sent=%u", hit.npc,
        hit.attacker, hit.amount, unsigned(hit.reason), hit.subpart, unsigned(sent));
  }
  for (const auto& blow : blows) {
    const bool sent = client->peer().send_npc_blow(blow);
    log("NPC blow sent npc=%u player=%u direction=%u sent=%u", blow.npc, blow.player,
        unsigned(blow.direction), unsigned(sent));
    if (blow.direction == integration::NpcBlowDirection::PlayerStruckNpc) {
      own_npc_blows[blow.npc] = {blow, local::clock_seconds()};
    }
  }
}

// The game gathers the player's lock-on candidates from the AI grid (WHGame 1.5.5:,
// the cone query), and a replica of an NPC ("NPC_NAI") has no AI object, so it was
// never a candidate. The game's own setting wh_cs_OpponentManagerBruteForce makes
// it go through every actor instead (()+616, read every update); the game's own NPCs
// are kept out of a session, so that is a short list. The enemy check that follows (the combat
// eligibility hook) lets a player lock on to a server NPC's body when it asks.
constexpr char kOpponentBruteForce[] = "wh_cs_OpponentManagerBruteForce";
bool opponent_brute_force_set = false;

void gather_every_opponent() {
  using GetCVar = void*(__fastcall*)(void* console, const char* name);
  using CVarSetInt = void(__fastcall*)(void* cvar, int value);
  constexpr std::size_t kConsoleGetCVar = 23;
  constexpr std::size_t kCVarSetInt = 7;
  void* console = console_slot ? *console_slot : nullptr;
  if (opponent_brute_force_set || !console) {
    return;
  }
  opponent_brute_force_set = true;
  void* cvar = slot<GetCVar>(console, kConsoleGetCVar)(console, kOpponentBruteForce);
  if (!cvar) {
    log("NPC %s missing: replicas of NPCs are no lock-on targets", kOpponentBruteForce);
    return;
  }
  slot<CVarSetInt>(cvar, kCVarSetInt)(cvar, 1);
  log("NPC %s=1: every actor is a lock-on candidate", kOpponentBruteForce);
}

// The local player asked to lock on: a replica of a living server NPC within reach in front of
// it is engaged (NPC_ENGAGE), so the server gives the NPC to this client and the game's own
// lock-on and fight take over (a replica runs no AI, and the game offers it as no target).
constexpr ULONGLONG kNpcLockAskMs = 500;
constexpr float kNpcEngageReach = 5;

// The game's lock-on filter also wants the actor's combat controller (actor + 632)
// and a soul its relations know (a replica has neither; the simulator's body gets the
// controller when its fight begins), so, as in the game, an NPC is a target once it fights.
void engage_npc_on_lock(void* entity_system, const float* view) {
  const ULONGLONG asked = npc_lock_asked_ms.exchange(0);
  if (!asked || GetTickCount64() - asked > kNpcLockAskMs || !view) {
    return;
  }
  // The camera's place and its forward axis (the view matrix's columns, as the speech uses).
  const core::Vec3 eye{view[3], view[7], view[11]};
  const core::Vec3 forward{view[1], view[5], view[9]};
  core::EntityId best = 0;
  float best_score = 0;
  for (const auto& [id, npc] : native_npcs) {
    // One this client simulates too: the server may let it fight back (an animal).
    void* entity = npc.killed ? nullptr : get_entity(entity_system, npc.engine_id);
    if (!entity) {
      continue;
    }
    const float* tm = slot<const float*(__fastcall*)(void*)>(entity, 32)(entity);
    const core::Vec3 to{tm[3] - eye.x, tm[7] - eye.y, tm[11] + 1.0f - eye.z};
    const float distance = std::sqrt(to.x * to.x + to.y * to.y + to.z * to.z);
    if (distance > kNpcEngageReach || distance < 0.01f) {
      continue;
    }
    // In front of the camera; the nearer to its axis and to the player, the better.
    const float facing = (to.x * forward.x + to.y * forward.y + to.z * forward.z) / distance;
    if (facing < 0.5f) {
      continue;
    }
    const float score = facing - distance * 0.05f;
    if (!best || score > best_score) {
      best = id;
      best_score = score;
    }
  }
  if (!best) {
    return;
  }
  bool sent = false;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    sent = client->peer().send_npc_engage(best);
  }
  log("NPC engage npc=%u on lock sent=%u", best, unsigned(sent));
}

// Reports how the walks of the NPCs simulated here ended. A walk replaced by the next order is
// not reported (the order it carried out is no longer the NPC's), nor a follow's walk (a follow
// reports its catch-up itself, tick_npc_follow) or a fight's (tick_npc_fight).
void report_finished_walks(void* entity_system, double now) {
  namespace walk = kcds::runtime::npcs::walk;
  for (const auto& done : walk::take_finished()) {
    if (native_horses.count(done.npc)) {
      // A horse's order walks the same way (riding/native_horses).
      horse_walk_finished(done.npc, done.sequence, done.status, now);
      continue;
    }
    log("NPC walk ended npc=%u order=%u game status=%u", done.npc, done.sequence,
        unsigned(done.status));
    const auto found = native_npcs.find(done.npc);
    if (done.status == walk::kWalkReplaced || found == native_npcs.end() ||
        !found->second.authority) {
      continue;
    }
    NativeNpc& npc = found->second;
    if (npc.order.kind == "follow" && npc.order.sequence == done.sequence) {
      npc.following = false;
      continue;
    }
    // The way around a player ended: on to the order's goal (tick_npc_avoid).
    if (npc.detouring && npc.order.sequence == done.sequence) {
      npc.detouring = false;
      if (void* entity = entity_system ? get_entity(entity_system, npc.engine_id) : nullptr) {
        const float* tm = slot<const float*(__fastcall*)(void*)>(entity, 32)(entity);
        const float from[3] = {tm[3], tm[7], tm[11]};
        const float goal[3] = {npc.walk_goal.x, npc.walk_goal.y, npc.walk_goal.z};
        if (walk::walk_to(entity, done.npc, done.sequence, from, goal, npc.order.speed)) {
          continue;
        }
      }
    }
    // A fight's walks close in or step back; the attack goes on (the next fight step walks
    // again when the target is out of reach).
    if (npc.order.kind == "attack" && npc.order.sequence == done.sequence) {
      npc.fight_walking = false;
      continue;
    }
    const auto status = done.status == walk::kWalkReached  ? integration::kNpcIntentReached
                        : done.status == walk::kWalkNoPath ? integration::kNpcIntentFailed
                                                           : integration::kNpcIntentBlocked;
    send_npc_intent_done(done.npc, npc.epoch, done.sequence, status);
  }
}

// The NPC's body was just made here as its simulator's: the local player's blow that started
// the fight, if it was a moment ago, lands on it too.
void replay_own_npc_blow(core::EntityId id, const NativeNpc& npc, double now) {
  const auto found = own_npc_blows.find(id);
  if (found == own_npc_blows.end()) {
    return;
  }
  const auto [blow, sent_at] = found->second;
  own_npc_blows.erase(found);
  const auto* local = static_cast<const std::uint8_t*>(local_combat_actor.load());
  void* rules = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(get_services()) + 416);
  if (now - sent_at > kOwnBlowReplaySeconds || !local || !rules) {
    return;
  }
  auto& hit = npc_blow_hits[next_npc_blow_hit];
  next_npc_blow_hit = (next_npc_blow_hit + 1) % npc_blow_hits.size();
  hit = blow.hit;
  const unsigned source = *reinterpret_cast<const unsigned*>(local + 48);
  const unsigned target = npc.engine_id;
  const float none = 0;
  std::memcpy(hit.data(), &source, sizeof(source));
  std::memcpy(hit.data() + 40, &target, sizeof(target));
  std::memcpy(hit.data() + 48, &none, sizeof(none));
  replaying_melee = true;
  original_queue_native_hit(rules, hit.data());
  replaying_melee = false;
  log("NPC fight npc=%u: the blow that started it played on the new body", id);
}

// Blows the other side of a fight sent (NPC_BLOW), played by this game with its own
// entities: a player's blow on the NPC this client simulates (its body reacts and its AI learns
// the striker; the health stays the server's), or an NPC's blow on the local player (its game
// takes the health, reacts, and reports playerDamage).
void apply_npc_blows() {
  std::vector<integration::NpcBlow> blows;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    blows = client->peer().take_npc_blows();
  }
  if (blows.empty()) {
    return;
  }
  void* rules = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(get_services()) + 416);
  const auto* local = static_cast<const std::uint8_t*>(local_combat_actor.load());
  for (const auto& blow : blows) {
    const auto npc = native_npcs.find(blow.npc);
    if (!rules || npc == native_npcs.end()) {
      continue;
    }
    auto& hit = npc_blow_hits[next_npc_blow_hit];
    next_npc_blow_hit = (next_npc_blow_hit + 1) % npc_blow_hits.size();
    hit = blow.hit;
    unsigned source = 0;
    unsigned target = 0;
    const bool on_npc = blow.direction == integration::NpcBlowDirection::PlayerStruckNpc;
    if (on_npc) {
      const auto remote = remotes.find(blow.player);
      if (!npc->second.authority || remote == remotes.end()) {
        continue;
      }
      source = remote->second.engine_id;
      target = npc->second.engine_id;
      // The striker's soul (its WUID at + 48) may be fought back (npc_attack.hpp struck_by), for
      // a combat blow while they stand.
      void* soul =
          remote->second.actor
              ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(remote->second.actor) + 1640)
              : nullptr;
      if (soul && kcds::runtime::npcs::attack::fight_back_hit(hit.data()) &&
          !kcds::runtime::knockout::replica_down(blow.player)) {
        kcds::runtime::npcs::attack::struck_by(
            blow.npc,
            *reinterpret_cast<const std::uint64_t*>(static_cast<std::uint8_t*>(soul) + 48));
        // Fighting another, the NPC turns on the striking player.
        note_player_strike(blow.npc, blow.player);
      }
      const float none = 0;
      std::memcpy(hit.data() + 48, &none, sizeof(none));
    } else {
      if (!local) {
        continue;
      }
      source = npc->second.engine_id;
      target = *reinterpret_cast<const unsigned*>(local + 48);
    }
    std::memcpy(hit.data(), &source, sizeof(source));
    std::memcpy(hit.data() + 40, &target, sizeof(target));
    original_queue_native_hit(rules, hit.data());
    if (!on_npc) {
      queue_hit_damage(hit.data());
    }
    float health = 0;
    std::memcpy(&health, blow.hit.data() + 48, sizeof(health));
    log("NPC blow played npc=%u player=%u on_npc=%u health=%.2f", blow.npc, blow.player,
        unsigned(on_npc), health);
  }
}

// The NPC's death on its body: the game's own lethal damage, once; the body then lies, and its
// movement and clips are no longer captured or replayed. A replica (the class without AI) is
// also given the game's ragdoll profile: the lethal damage only starts the mortally wounded loop,
// and the NPC's brain, which a replica lacks, takes the body from there to its fall; without it
// the replica stood hunched for good while its simulator's body lay dead.
void keep_npc_dead(core::EntityId id, NativeNpc& npc) {
  if (!npc.killed && npc.soul) {
    kcds::runtime::npcs::health::kill(npc.soul);
    npc.killed = true;
    const bool fell =
        !npc.authority && npc.actor && set_physics_profile(npc.actor, PhysicsProfile::Ragdoll);
    log("NPC died npc=%u authority=%u health=%.2f ragdoll=%u", id, unsigned(npc.authority),
        kcds::runtime::npcs::health::health_of(npc.soul), unsigned(fell));
  }
  if (npc.skeleton) {
    std::lock_guard<std::shared_mutex> lock(motion_mutex);
    horse_captures.erase(npc.skeleton);
    remote_inputs.erase(npc.skeleton);
  }
}

// The dead body lies the same way on every screen: its simulator's game reports how
// its body lies once it lies still (NPC_POSE), and every replica lays its own ragdoll out that way.
// Each game's ragdoll falls on its own (a replica's from the ragdoll profile keep_npc_dead gives
// it), so the bodies lay in different poses, one face down and one on its back (kit).
constexpr double kNpcPoseAfterSeconds = 3.0;
constexpr double kNpcPoseGiveUpSeconds = 10.0;

void lay_npc_body(core::EntityId id, NativeNpc& npc, void* entity, const std::string& reported,
                  double now) {
  if (!entity) {
    return;
  }
  if (npc.died_at < 0) {
    npc.died_at = now;
  }
  if (npc.authority) {
    const double lying = now - npc.died_at;
    if (npc.pose_sent || lying < kNpcPoseAfterSeconds || lying > kNpcPoseGiveUpSeconds) {
      return;
    }
    const std::string parts = capture_ragdoll_pose(entity);
    const float* tm = slot<const float*(__fastcall*)(void*)>(entity, 32)(entity);
    if (!valid_ragdoll_pose(parts) || !tm) {
      return;
    }
    const core::Quat q = rotation(tm);
    const float header[7] = {tm[3], tm[7], tm[11], q.x, q.y, q.z, q.w};
    std::string pose(reinterpret_cast<const char*>(header), sizeof(header));
    pose += parts;
    bool sent = false;
    {
      std::lock_guard<std::mutex> lock(network_mutex);
      sent = client->peer().send_npc_pose(id, pose);
    }
    npc.pose_sent = sent;
    log("NPC pose sent npc=%u bytes=%zu sent=%u", id, pose.size(), unsigned(sent));
    return;
  }
  if (reported == npc.pose_applied || !integration::valid_npc_pose(reported)) {
    return;
  }
  float header[7] = {};
  std::memcpy(header, reported.data(), sizeof(header));
  const std::string parts = reported.substr(integration::kNpcPoseHeader);
  if (!valid_ragdoll_pose(parts) || !articulated_physics(entity)) {
    return;
  }
  set_transform(entity, core::Vec3{header[0], header[1], header[2]},
                core::Quat{header[3], header[4], header[5], header[6]}, 0);
  const unsigned laid = apply_ragdoll_pose(entity, parts);
  if (laid) {
    npc.pose_applied = reported;
  }
  log("NPC pose laid out npc=%u parts=%u", id, laid);
}

// The living NPCs with a body here, for npc_bodies().
std::vector<NpcBody> npc_body_list;

template <class Entities>
void collect_npc_bodies(const Entities& entities) {
  npc_body_list.clear();
  for (const auto& [id, npc] : native_npcs) {
    if (!npc.engine_id || npc.killed) {
      continue;
    }
    const auto found = entities.find(id);
    if (found == entities.end() || found->second.fields.size() <= integration::NpcAlive) {
      continue;
    }
    const auto& fields = found->second.fields;
    if (!std::get<bool>(fields[integration::NpcAlive])) {
      continue;
    }
    NpcBody body;
    body.id = id;
    body.engine_id = npc.engine_id;
    body.position = std::get<core::Vec3>(fields[integration::NpcPosition]);
    body.name = std::get<std::string>(fields[integration::NpcName]);
    npc_body_list.push_back(std::move(body));
  }
  std::sort(npc_body_list.begin(), npc_body_list.end(),
            [](const NpcBody& a, const NpcBody& b) { return a.id < b.id; });
}

// The removed bodies the game kept past kRemovalCheckSeconds go at once.
void remove_kept_bodies(void* entity_system) {
  const auto now = std::chrono::steady_clock::now();
  for (auto it = bodies_removed.begin(); it != bodies_removed.end();) {
    const double waited = std::chrono::duration<double>(now - it->since).count();
    if (waited < kRemovalCheckSeconds) {
      ++it;
      continue;
    }
    bool shown = false;
    for (const auto& [id, npc] : native_npcs) {
      if (npc.engine_id == it->engine) {
        shown = true;
        break;
      }
    }
    if (entity_system && !shown && get_entity(entity_system, it->engine)) {
      log("NPC body kept by the game network=%u engine=0x%x after %.1fs: removed now",
          unsigned(it->id), it->engine, waited);
      slot<void(__fastcall*)(void*, std::uint32_t, bool)>(entity_system, 19)(entity_system,
                                                                             it->engine, true);
    }
    it = bodies_removed.erase(it);
  }
}

template <class Entities>
void retire_native_npcs(const Entities& entities, void* entity_system) {
  remove_kept_bodies(entity_system);
  for (auto it = native_npcs.begin(); it != native_npcs.end();) {
    if (entities.count(it->first)) {
      ++it;
      continue;
    }
    remove_npc_body(entity_system, it->first, it->second);
    npc_retry_at.erase(it->first);
    it = native_npcs.erase(it);
  }
  for (auto it = npc_retry_at.begin(); it != npc_retry_at.end();) {
    if (!entities.count(it->first)) {
      it = npc_retry_at.erase(it);
    } else {
      ++it;
    }
  }
  // A body this client no longer shows says nothing here.
  for (auto it = npc_speeches.begin(); it != npc_speeches.end();) {
    if (!native_npcs.count(it->first)) {
      kcds::runtime::npcs::end_speech(it->second);
      it = npc_speeches.erase(it);
    } else {
      ++it;
    }
  }
}

template <class Entities>
void tick_native_npcs(const Entities& entities, void* actor_system, void* entity_system,
                      const float* view, double now) {
  const bool measure_combat = npc_combat_metrics_enabled();
  NpcCombatCounts combat_counts;
  const auto own = local_network_id.load();
  // Each step is timed (frames of 16 and 19 s in the NPC tick when a second
  // player came into a village or struck an NPC the first one fought).
  {
    const SlowNpcWatch watch("barks", 0);
    send_npc_barks(entity_system);
  }
  {
    const SlowNpcWatch watch("hits", 0);
    send_npc_hits();
  }
  {
    const SlowNpcWatch watch("blows", 0);
    apply_npc_blows();
  }
  {
    const SlowNpcWatch watch("opponents", 0);
    gather_every_opponent();
  }
  {
    const SlowNpcWatch watch("lock", 0);
    engage_npc_on_lock(entity_system, view);
  }
  {
    const SlowNpcWatch watch("walks", 0);
    report_finished_walks(entity_system, now);
  }
  const SlowNpcWatch loop_watch("bodies and after", 0);
  for (const auto& [id, e] : entities) {
    if (e.type != core::type_id("NetworkNpc") || e.fields.size() <= integration::NpcLife) {
      continue;
    }
    // One NPC that holds the frame long is told with the stage it was in (a
    // frame of 16 s in the NPC tick as a second player came into a village).
    const SlowNpcWatch watch("body", std::uint32_t(id));
    const auto life = std::get<std::int64_t>(e.fields[integration::NpcLife]);
    const bool alive = std::get<bool>(e.fields[integration::NpcAlive]);
    const float health = std::get<float>(e.fields[integration::NpcHealth]);
    // The share of their damage its blows on players take (Npc.setCombatSkill({ damage })): the
    // struck player's game applies it (player_damage.cpp).
    if (e.fields.size() > integration::NpcDamage) {
      kcds::runtime::npcs::health::set_damage_share(
          std::uint32_t(id), std::get<float>(e.fields[integration::NpcDamage]));
    }
    const auto p = std::get<core::Vec3>(e.fields[integration::NpcPosition]);
    const auto q = std::get<core::Quat>(e.fields[integration::NpcRotation]);
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
      continue;
    }
    const auto& soul = std::get<std::string>(e.fields[integration::NpcSoul]);
    const auto& actor_class = std::get<std::string>(e.fields[integration::NpcActorClass]);
    const auto simulator =
        core::EntityId(std::get<std::int64_t>(e.fields[integration::NpcSimulator]));
    const auto epoch = std::uint32_t(std::get<std::int64_t>(e.fields[integration::NpcEpoch]));
    const bool authority = simulator == own && own != 0;
    // A woman's replica is the class without AI made a woman (npc_body.hpp spawn_body).
    const bool female_replica = actor_class == "NPC_Female" && !authority;
    // An animal has no class without AI: its replica is its own class, replaying its simulator
    // like a horse's.
    const bool animal = integration::npc_animal_class(actor_class);
    const char* body_class = authority || animal ? actor_class.c_str() : kNpcReplicaClass;
    auto it = native_npcs.find(id);
    // The level's own person (issue 016): its body is that person's entity in this game.
    const std::string level_guid = e.fields.size() > integration::NpcLevelGuid
                                       ? std::get<std::string>(e.fields[integration::NpcLevelGuid])
                                       : std::string{};
    if (!level_guid.empty() && it != native_npcs.end() && it->second.level &&
        get_entity(entity_system, it->second.engine_id) && it->second.authority != authority) {
      // Another game runs it now: the same entity, its brain on or off. Taken over here, the body
      // gets its own AI LOD state back, which the replica held off.
      it->second.authority = authority;
      it->second.brain_checked = false;
      void* level_entity = get_entity(entity_system, it->second.engine_id);
      void* level_agent =
          level_entity ? kcds::runtime::npcs::walk::agent_of(level_entity) : nullptr;
      if (authority && level_agent && it->second.level_lod != 255) {
        *(static_cast<std::uint8_t*>(level_agent) + 1800) = it->second.level_lod;
      }
      kcds::runtime::npcs::health::Body tracked;
      tracked.npc = std::uint32_t(id);
      tracked.simulated = authority;
      kcds::runtime::npcs::health::track(it->second.engine_id, it->second.soul, tracked);
      if (!authority) {
        give_npc_combat_actor(id, it->second.actor);
      }
      log("NPC level body npc=%u local=%u lod=%u", id, unsigned(authority),
          level_agent ? unsigned(*(static_cast<std::uint8_t*>(level_agent) + 1800)) : 255u);
    }
    // The game removed the body itself, or this client's part changed: the body goes.
    const bool lost = it != native_npcs.end() && !get_entity(entity_system, it->second.engine_id);
    const bool other_part = it != native_npcs.end() && it->second.authority != authority;
    // Revived: the dead body goes and a living one is made where the NPC is.
    const bool revived = it != native_npcs.end() && it->second.life != life;
    if (lost || other_part || revived || (it != native_npcs.end() && !body_class)) {
      // A level person's entity stays the level's (it is only let go): nothing to wait for.
      const std::uint32_t old_engine = it->second.level ? 0 : it->second.engine_id;
      remove_npc_body(entity_system, id, it->second);
      native_npcs.erase(it);
      it = native_npcs.end();
      // The entity system takes the body away at the end of the frame. A new body made with the
      // same soul before then shared the soul with the body that went, and the soul it freed stayed
      // in the game's soul grid (wh::rpgmodule::Souls: a crash 62 ms after a
      // hand-over, reading it). The new body waits until the old one is gone.
      if (!lost && old_engine != 0) {
        bodies_going[id] = BodyGoing{old_engine, now};
      }
    }
    if (const auto going = bodies_going.find(id); going != bodies_going.end()) {
      const bool waited_out = now - going->second.since >= kBodyGoingSeconds;
      if (!waited_out && get_entity(entity_system, going->second.engine)) {
        continue;
      }
      bodies_going.erase(going);
    }
    if (!body_class) {
      continue;
    }
    if (it == native_npcs.end() && !level_guid.empty()) {
      // The level's person, once this game has their entity.
      const std::uint32_t engine_id = kcds::runtime::npcs::level_person_entity(level_guid);
      NativeNpc bound;
      if (engine_id == 0 ||
          !bind_level_body(entity_system, actor_system, id, engine_id, authority, bound)) {
        continue;
      }
      bound.life = life;
      it = native_npcs.emplace(id, bound).first;
    }
    if (it == native_npcs.end()) {
      if (!character_spawns_ready()) {
        continue;
      }
      if (npc_retry_at.count(id) && now < npc_retry_at[id]) {
        continue;
      }
      npc_retry_at[id] = now + 5;
      NativeNpc made;
      if (!make_npc_body(actor_system, id, p, q, soul, body_class, authority, female_replica,
                         made)) {
        continue;
      }
      made.life = life;
      npc_retry_at.erase(id);
      it = native_npcs.emplace(id, made).first;
      if (authority && alive) {
        replay_own_npc_blow(id, it->second, now);
      }
    }
    auto& npc = it->second;
    void* entity = get_entity(entity_system, npc.engine_id);
    // A level person's actor and soul can come after the entity (the level gives them later):
    // read again until they are there, so the prompts, blows and health find the body.
    if (npc.level && entity) {
      if (!npc.actor) {
        npc.actor = slot<void*(__fastcall*)(void*, std::uint32_t)>(actor_system, 3)(actor_system,
                                                                                    npc.engine_id);
      }
      void* soul = npc.actor
                       ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(npc.actor) + 1640)
                       : nullptr;
      if (soul != npc.soul) {
        npc.soul = soul;
        kcds::runtime::npcs::health::Body tracked;
        tracked.npc = std::uint32_t(id);
        tracked.simulated = npc.authority;
        kcds::runtime::npcs::health::track(npc.engine_id, npc.soul, tracked);
        log("NPC level body npc=%u soul=%p", id, soul);
      }
    }
    // The body's skeleton, read anew every frame: dressing the body (an outfit applied) can give it
    // a new character, and the old skeleton is then gone. Its capture and replay move with it.
    void* skeleton = nullptr;
    if (entity) {
      if (void* character = slot<void*(__fastcall*)(void*, unsigned)>(entity, 101)(entity, 0)) {
        skeleton = slot<void*(__fastcall*)(void*)>(character, 5)(character);
      }
    }
    if (skeleton != npc.skeleton) {
      if (npc.skeleton) {
        std::lock_guard<std::shared_mutex> lock(motion_mutex);
        horse_captures.erase(npc.skeleton);
        remote_inputs.erase(npc.skeleton);
        animation_stop_fences.erase(npc.skeleton);
      }
      log("NPC skeleton npc=%u %p -> %p", id, npc.skeleton, skeleton);
      npc.skeleton = skeleton;
      std::memset(npc.animation_sequences, 0, sizeof(npc.animation_sequences));
      // The resource's clip plays again on the new skeleton, and its order goes on.
      npc.animation.clear();
      npc.intent.clear();
    }
    if (!npc.animated && npc.actor) {
      npc.animated = slot<void*(__fastcall*)(void*)>(npc.actor, 90)(npc.actor);
    }
    if (epoch != npc.epoch) {
      npc.epoch = epoch;
      std::memset(npc.animation_sequences, 0, sizeof(npc.animation_sequences));
      log("NPC authority npc=%u simulator=%u epoch=%u local=%u", id, simulator, epoch,
          unsigned(authority));
    }
    update_npc_speech(id, std::get<std::string>(e.fields[integration::NpcSpeech]),
                      std::get<std::string>(e.fields[integration::NpcName]), entity, view, now);
    // The resource's look, on every body of the NPC (the soul's own until one is chosen).
    const auto& look = std::get<std::string>(e.fields[integration::NpcAppearance]);
    if (!look.empty() && look != npc.appearance && npc.actor) {
      apply_npc_appearance(id, npc.actor, look);
      npc.appearance = look;
    }
    // The carcass's Search and Gut.
    const bool carcass_closed = e.fields.size() > integration::NpcCarcassClosed &&
                                std::get<bool>(e.fields[integration::NpcCarcassClosed]);
    if (carcass_closed != kcds::runtime::npcs::health::carcass_closed(id)) {
      kcds::runtime::npcs::health::set_carcass_closed(id, carcass_closed);
      log("NPC carcass closed npc=%u %u", id, unsigned(carcass_closed));
    }
    if (!alive) {
      // A body made after the death (a late joiner, a new simulator) wears what the NPC wore.
      const auto& outfit = std::get<std::string>(e.fields[integration::NpcOutfit]);
      if (!animal && !outfit.empty() && npc.actor && entity) {
        kcds::runtime::appearance::outfit::apply(id, npc.actor, entity, outfit);
      }
      keep_npc_dead(id, npc);
      if (e.fields.size() > integration::NpcRagdollPose) {
        lay_npc_body(id, npc, entity, std::get<std::string>(e.fields[integration::NpcRagdollPose]),
                     now);
      }
      continue;
    }
    kcds::runtime::npcs::health::show_health(npc.soul, health);
    // Npc.interactable, on every client: the game's Talk, Chat and Reply prompts leave its body.
    const bool interactable = e.fields.size() <= integration::NpcInteractable ||
                              std::get<bool>(e.fields[integration::NpcInteractable]);
    if (interactable != kcds::runtime::npcs::health::interactable(id)) {
      kcds::runtime::npcs::health::set_interactable(id, interactable);
      log("NPC interactable npc=%u %u", id, unsigned(interactable));
    }
    // Npc.attackable, on every client: players' hits on it are dropped.
    const bool attackable = e.fields.size() <= integration::NpcAttackable ||
                            std::get<bool>(e.fields[integration::NpcAttackable]);
    if (attackable != kcds::runtime::npcs::health::attackable(id)) {
      kcds::runtime::npcs::health::set_attackable(id, attackable);
      log("NPC attackable npc=%u %u", id, unsigned(attackable));
    }
    // Npc.targetable, on every client: the lock-on never offers the body.
    const bool targetable = e.fields.size() <= integration::NpcTargetable ||
                            std::get<bool>(e.fields[integration::NpcTargetable]);
    if (targetable != kcds::runtime::npcs::health::targetable(id)) {
      kcds::runtime::npcs::health::set_targetable(id, targetable);
      log("NPC targetable npc=%u %u", id, unsigned(targetable));
    }
    if (npc.skeleton) {
      // The capture of what the body does (the simulator's) and its replay (everyone else's) are
      // the horses', by skeleton.
      std::lock_guard<std::shared_mutex> lock(motion_mutex);
      auto& captured = horse_captures[npc.skeleton];
      captured.animated = npc.animated;
      captured.authority = authority;
      if (authority) {
        remote_inputs.erase(npc.skeleton);
      }
    }
    // Npc.setSpeaking: the style's face clip loops on the body in every game.
    if (e.fields.size() > integration::NpcTalking) {
      const bool previous_replay = replaying_animation;
      replaying_animation = true;
      kcds::runtime::player_animation::keep_replica_talking(
          id, alive ? entity : nullptr, std::get<std::string>(e.fields[integration::NpcTalking]));
      replaying_animation = previous_replay;
    }
    const auto teleport = std::get<std::int64_t>(e.fields[integration::NpcTeleport]);
    if (authority) {
      // A teleport of the server's puts the body there; the first count is only taken.
      if (npc.teleport >= 0 && teleport != npc.teleport && entity) {
        set_transform(entity, p, q);
        log("NPC teleport npc=%u to (%.2f %.2f %.2f)", id, p.x, p.y, p.z);
      }
      npc.teleport = teleport;
      // The resource's clip, when it changed (also the one it had when this body took over).
      const auto& animation = std::get<std::string>(e.fields[integration::NpcAnimation]);
      if (animation != npc.animation && entity && npc.skeleton) {
        apply_npc_animation(id, npc, entity, animation);
        npc.clip_started = 0;
        npc.animation = animation;
      }
      // Npc.locomotion: a change carries the order out again the new way.
      const bool kinematic = e.fields.size() > integration::NpcKinematic &&
                             std::get<bool>(e.fields[integration::NpcKinematic]);
      if (kinematic != npc.kinematic) {
        npc.kinematic = kinematic;
        npc.intent.clear();
        log("NPC locomotion npc=%u %s", id, kinematic ? "kinematic" : "native");
      }
      // The resource's order, when it changed (also the one it had when this body took over).
      const auto& intent = std::get<std::string>(e.fields[integration::NpcIntent]);
      if (intent != npc.intent && entity && alive && now >= npc.order_retry_at) {
        npc.order_retry_at = 0.0;
        apply_npc_intent(id, npc, entity, intent);
        // Refused while the body is new: the same order comes again.
        if (npc.order_retry_at == 0.0) {
          npc.intent = intent;
        }
      }
      // Npc.setExpression: the simulator's body holds it, the clip sync shows
      // it.
      if (entity && npc.skeleton && e.fields.size() > integration::NpcFace) {
        const auto& wanted = std::get<std::string>(e.fields[integration::NpcFace]);
        if (wanted != npc.face) {
          npc.face = wanted;
          const auto bar = wanted.find('|');
          const std::string fragment = wanted.substr(0, bar);
          const std::string tags = bar == std::string::npos ? "" : wanted.substr(bar + 1);
          void* controller = kcds::runtime::body_state::action_controller(npc.actor);
          const bool queued =
              kcds::runtime::player_animation::face_on_body(id, controller, fragment, tags);
          log("NPC face npc=%u %s tags '%s' %s", id, fragment.c_str(), tags.c_str(),
              queued ? "held" : "refused (no such fragment)");
        }
      }
      // Npc.brain, as the server last said.
      const bool brain_on = e.fields.size() <= integration::NpcBrain ||
                            std::get<bool>(e.fields[integration::NpcBrain]);
      if (brain_on != npc.brain_on) {
        npc.brain_on = brain_on;
        log("NPC brain npc=%u %s", id, brain_on ? "on" : "off");
      }
      if (entity && alive && !npc.brain_on) {
        // The game's AI LOD state of its agent, 2 is off: the body does not think or fight.
        if (void* agent = kcds::runtime::npcs::walk::agent_of(entity)) {
          *(static_cast<std::uint8_t*>(agent) + 1800) = 2;
          // The game's own AI switch too: the LOD state alone left its perception and
          // behaviours running (picking items up, remarks on a player). Checked every frame, as
          // the NPC manager may activate a new body after it was spawned; a brain switched on again
          // is activated again.
          npc.brain_checked = false;
          const auto flags = kcds::runtime::npcs::attack::deactivate_brain(agent);
          if (flags.before != flags.after) {
            log("NPC brain npc=%u agent=%p off: flags 0x%x -> 0x%x active 0x%x", id, agent,
                unsigned(flags.before), unsigned(flags.after), unsigned(flags.active));
          }
        }
      }
      if (entity && alive && kcds::runtime::npcs::real_movement::keep_real(npc.engine_id)) {
        // The game had sent this body's walks to its fake movement (issue 016).
        ++npc.real_movement_restored;
        if (npc.real_movement_restored == 1 || npc.real_movement_restored % 100 == 0) {
          log("NPC real movement npc=%u engine=0x%x taken back from fake movement (%u times)", id,
              npc.engine_id, npc.real_movement_restored);
        }
      }
      if (entity && alive && npc.brain_on && !npc.brain_checked) {
        // A body spawned at runtime may be left unactivated by the NPC manager, so its brain
        // never runs (no fights, no ambient behaviour): activated here once.
        npc.brain_checked = true;
        void* agent = kcds::runtime::npcs::walk::agent_of(entity);
        const auto flags = kcds::runtime::npcs::attack::activate_brain(agent);
        // Its brain fights only whom the server says (Npc.attack), not on its faction's account.
        kcds::runtime::npcs::attack::govern(agent, id);
        log("NPC brain npc=%u agent=%p flags 0x%x -> 0x%x active 0x%x", id, agent,
            unsigned(flags.before), unsigned(flags.after), unsigned(flags.active));
      }
      if (const unsigned refused = kcds::runtime::npcs::attack::take_refused_requests()) {
        log("NPC brain fights refused: %u not ordered by the server", refused);
      }
      if (const auto stats = kcds::runtime::npcs::attack::take_request_stats();
          stats.allowed || stats.forgiven) {
        log("NPC brain fights allowed=%u last target=0x%llx forgiven=%u", stats.allowed,
            static_cast<unsigned long long>(stats.last_target), stats.forgiven);
      }
      if (entity && alive) {
        tick_npc_follow(entities, id, npc, entity, now);
        tick_npc_avoid(entities, id, npc, entity, now);
        tick_npc_glide(id, npc, entity, now);
        tick_npc_clip(id, npc, entity, now);
        tick_npc_look(entities, npc, entity, now);
        tick_npc_attack(entity_system, id, npc, entity, now);
        tick_npc_fight(entities, id, npc, entity, now);
      }
      if (!entity || !npc.skeleton) {
        continue;
      }
      // While the body moves, turns or fights its state goes out twenty times a second, so the
      // replicas follow closely (movement sometimes laggy at ten); while it stands still,
      // five times a second, which keeps a crowd of animals cheap.
      const float* tm = slot<const float*(__fastcall*)(void*)>(entity, 32)(entity);
      const core::Vec3 here{tm[3], tm[7], tm[11]};
      const core::Quat facing = rotation(tm);
      const auto combat_mode = kcds::runtime::npcs::observe_combat_mode(npc.actor, npc_combat_mode);
      const bool fighting = kcds::runtime::npcs::combat_needs_fast_sync(combat_mode);
      if (measure_combat) {
        combat_counts.add(combat_mode);
      }
      const float dx = here.x - npc.sent_position.x;
      const float dy = here.y - npc.sent_position.y;
      const float dz = here.z - npc.sent_position.z;
      const float turn = std::fabs(facing.x * npc.sent_rotation.x + facing.y * npc.sent_rotation.y +
                                   facing.z * npc.sent_rotation.z + facing.w * npc.sent_rotation.w);
      if (fighting || dx * dx + dy * dy + dz * dz > kNpcMovedSquared || turn < kNpcTurnedDot) {
        // A little past the last move, so the replicas get the stop and its velocity of zero.
        npc.active_until = now + kNpcActiveTailSeconds;
      }
      const bool active = now < npc.active_until;
      const double since = now - npc.reported_at;
      if (since < (active ? kNpcActiveReportSeconds : kNpcIdleReportSeconds)) {
        continue;
      }
      npc.reported_at = now;
      trace_sent_state(id, npc, here, entity, now);
      // A door this body works is told as the NPC's use (issue 016).
      kcds::runtime::doors::note_simulated_npc(id, here.x, here.y, here.z);
      npc.sent_position = here;
      npc.sent_rotation = facing;
      capture_existing_animations(npc.skeleton, true);
      integration::NpcState state;
      state.revision = requested_revision;
      state.epoch = epoch;
      state.source = own;
      state.npc = id;
      state.sequence = ++npc.sequence;
      state.position = here;
      state.rotation = facing;
      {
        std::lock_guard<std::shared_mutex> lock(motion_mutex);
        const auto& c = horse_captures.at(npc.skeleton);
        state.velocity = c.motion.velocity;
        state.motion_rotation = c.motion.rotation;
        state.motion_type = c.motion.type;
        // Armed while our attack drew the weapon or the body holds one in a fight (its brain drew
        // it in a fight of its own: the replicas showed the sword in its sheath). Out of a
        // fight a hand holds a spoon, a bowl or a tool, which is no weapon: taken as one, every
        // replica drew its axe and ate with it (issue 016).
        const bool holds_item = kcds::runtime::weapons::item_in_hand(npc.actor, false) ||
                                kcds::runtime::weapons::item_in_hand(npc.actor, true);
        const bool armed = npc.weapon_drawn || (fighting && holds_item);
        // Seated at a dice table, or using a seat, bench or other smart object of the level
        // (issue 016: on another game a housemaid on a bench jumped, pushed up by her capsule).
        // The game lets go of the seat and takes it again between the parts of what the NPC
        // does on it, and the head rises as it gestures: both held for kSeatGraceSeconds, or the
        // replica stood up and sat down again every few seconds.
        const bool holds_now = kcds::runtime::seats::holds_seat(npc.engine_id);
        if (holds_now) {
          npc.seat_held_at = now;
        }
        const bool uses_seat =
            holds_now || (npc.seat_held_at > 0 && now - npc.seat_held_at < kSeatGraceSeconds);
        const bool seated_at_dice =
            (kcds::runtime::dice::seated_npc(npc.skeleton) && kcds::runtime::dice::at_table()) ||
            uses_seat;
        // Sitting on it, not standing at it (a workbench): the head is low above the feet when it
        // sits down, and it stays sitting while it keeps the seat.
        const bool sitting = uses_seat && (npc.sitting_sent || head_low(entity, here));
        npc.sitting_sent = sitting;
        state.motion_flags = (c.motion.allow_strafe ? 1 : 0) | (c.motion.jumping ? 2 : 0) |
                             (fighting ? integration::kBodyFighting : 0) |
                             (armed ? integration::kBodyArmed : 0) |
                             (seated_at_dice ? integration::kBodySeated : 0) |
                             (sitting ? integration::kBodySitting : 0);
        state.motion_valid = c.valid;
        state.mask = c.mask;
        state.hands = armed ? std::string() : npc_hands_field(entity_system, entity, npc);
        if (state.hands != npc.sent_hands) {
          npc.sent_hands = state.hands;
          log("NPC hands npc=%u hold '%s'", id, state.hands.c_str());
        }
        std::memcpy(state.params.data(), c.params, sizeof(c.params));
        state.animations = local::encode_animation(c.animations);
      }
      // What the body wears, when it changed: the game dressed it anew for this soul, and the
      // replicas wear the same. A body that took over from another simulator first wears what
      // the server already has, so the NPC keeps its clothes when its simulator changes. An
      // outfit a resource chose (Npc.setOutfit) is worn as it is and nothing is reported.
      // How much of the coming attacks the body defends against (Npc.setCombatSkill).
      if (e.fields.size() > integration::NpcDefense) {
        kcds::runtime::npcs::attack::set_defense_chance(
            npc.actor, std::get<float>(e.fields[integration::NpcDefense]));
      }
      const auto& known = std::get<std::string>(e.fields[integration::NpcOutfit]);
      const bool fixed = std::get<bool>(e.fields[integration::NpcOutfitFixed]);
      if (!animal && fixed && npc.actor) {
        kcds::runtime::appearance::outfit::apply(id, npc.actor, entity, known);
      }
      std::string outfit;
      if (!animal && !fixed && now >= npc.next_outfit && npc.actor) {
        npc.next_outfit = now + kNpcOutfitSeconds;
        outfit = kcds::runtime::appearance::outfit::capture(npc.actor);
        if (!known.empty() && npc.sent_outfit.empty() && outfit != known) {
          kcds::runtime::appearance::outfit::apply(id, npc.actor, entity, known);
          outfit.clear();
        } else if (!known.empty() && npc.sent_outfit.empty()) {
          npc.sent_outfit = known;
        }
      }
      std::lock_guard<std::mutex> lock(network_mutex);
      client->peer().send_npc_state(std::move(state));
      if (!outfit.empty() && outfit != npc.sent_outfit &&
          client->peer().send_npc_outfit(id, outfit)) {
        npc.sent_outfit = outfit;
        log("NPC outfit sent npc=%u %s", id,
            kcds::runtime::appearance::outfit::describe(outfit).c_str());
      }
      // The face the game chose for this body, while the server has none: every other body of
      // the NPC is built from it (NPC_LOOK; the game picks a runtime body's face per client).
      if (!animal && !npc.look_sent && look.empty() && npc.actor && now >= npc.next_look) {
        npc.next_look = now + kNpcOutfitSeconds;
        const auto parts = kcds::runtime::appearance::components::built_from(npc.actor);
        if (parts.size() >= 4 && !parts[1].empty()) {
          const std::string chosen = parts[0] + "|" + parts[1] + "|" + parts[2] + "|" + parts[3];
          npc.look_sent = client->peer().send_npc_look(id, chosen);
          log("NPC look npc=%u %s %s", id, chosen.c_str(), npc.look_sent ? "sent" : "not sent");
        }
      }
      continue;
    }
    npc.teleport = teleport;
    // A level person's replica keeps its own brain off (the AI LOD state 2): the simulator's runs
    // it, this one only shows it.
    if (npc.level && entity) {
      if (void* agent = kcds::runtime::npcs::walk::agent_of(entity)) {
        *(static_cast<std::uint8_t*>(agent) + 1800) = 2;
      }
    }
    // The replica wears what the simulator's body wears (an animal wears nothing).
    if (!animal && npc.actor && entity) {
      const auto& outfit = std::get<std::string>(e.fields[integration::NpcOutfit]);
      kcds::runtime::appearance::outfit::apply(id, npc.actor, entity, outfit);
    }
    const auto& raw = std::get<std::string>(e.fields[integration::NpcNativeState]);
    integration::NpcState state;
    const bool state_valid =
        !raw.empty() &&
        integration::decode_npc_state(std::vector<std::uint8_t>(raw.begin(), raw.end()), state) &&
        state.epoch == epoch && state.source == simulator && state.revision == requested_revision;
    engine_stage = "NPC replica transform";
    // A seated NPC's replica does not stand on its chair: its capsule pushed it 45 cm up, and the
    // follow below pulled it down again, every few frames.
    if (state_valid) {
      const bool seated = (state.motion_flags & integration::kBodySeated) != 0;
      kcds::runtime::seats::collisions::keep(id, npc.engine_id, seated, false);
      // The simulator's body sits on a seat of the level (a bench, a chair): the replica holds the
      // game's "sitting" stance, as the seat holds it on the simulator's body. Without it the
      // replica's own locomotion stood its legs up in front of the bench while its upper body
      // played the sitting clips.
      const bool sitting = (state.motion_flags & integration::kBodySitting) != 0;
      if (sitting != npc.sitting_held && npc.actor) {
        void* controller = kcds::runtime::body_state::action_controller(npc.actor);
        if (kcds::runtime::player_animation::hold_global_tag(controller, "sitting", sitting)) {
          npc.sitting_held = sitting;
          log("NPC replica npc=%u %s", id, sitting ? "sits" : "stands up");
        }
      }
    }
    if (state_valid) {
      trace_received_state(id, npc, state, entity, now);
      // What the simulator's body holds (issue 016).
      if (entity) {
        kcds::runtime::animation_props::keep(id, entity, state.hands);
      }
    }
    if (state_valid && (state.motion_flags & integration::kBodyFighting)) {
      follow_fighting_npc(npc, entity, state.position, state.rotation, now);
    } else if (state_valid) {
      // Every body: the horse's follower lets a walking person drift a metre and turn before it
      // corrects, which showed as a laggy walk and a sit-down in jumps.
      follow_animal_npc(npc, entity, state, now);
    } else if (!npc.level) {
      follow_replicated_npc(npc, entity, p, q,
                            state_valid && state.motion_valid ? &state.velocity : nullptr, now);
    }
    // A level person no simulator has reported yet stays where this game's level has it: the
    // server's place for it is only where the level placed it, and many of the level's people
    // share one placement (the stud farm's: 18 within 5 m). Pulled there, they all stood in one
    // crowd, and the game that took them over started them from it;
    // the game puts them where their schedules have them, as in singleplayer.
    if (!state_valid) {
      continue;
    }
    // The simulator's body holds its weapon: this replica draws the one it carries too, once.
    const bool armed = (state.motion_flags & integration::kBodyArmed) != 0;
    if (armed && !npc.weapon_drawn && npc.actor) {
      npc.weapon_drawn = kcds::runtime::npcs::attack::draw_carried(npc.actor);
      npc.next_draw_check = now + kNpcDrawCheckSeconds;
    } else if (armed && npc.actor && now >= npc.next_draw_check) {
      // Armed with nothing in either hand: it draws again (a bow replica held nothing and
      // fired none of its simulator's shots).
      npc.next_draw_check = now + kNpcDrawCheckSeconds;
      if (!kcds::runtime::weapons::item_in_hand(npc.actor, false) &&
          !kcds::runtime::weapons::item_in_hand(npc.actor, true)) {
        // A bow first (the set draw does not take it), else the set the body carries.
        const bool bow = kcds::runtime::weapons::draw_set_item(
            npc.actor, kcds::runtime::ranged::is_missile_weapon);
        if (!bow) {
          kcds::runtime::npcs::attack::draw_carried(npc.actor);
        }
        log("NPC replica npc=%u armed with nothing in hand: draws %s", id,
            bow ? "its bow" : "again");
      }
    } else if (!armed && npc.weapon_drawn) {
      // The same call again puts it away (a second draw sheathed the sword).
      kcds::runtime::npcs::attack::draw_carried(npc.actor);
      npc.weapon_drawn = false;
    }
    auto clips = local::decode_animation(state.animations);
    RemoteInputs remote;
    remote.animated = npc.animated;
    remote.valid = state.motion_valid;
    remote.motion.type = state.motion_type;
    remote.motion.velocity = state.velocity;
    remote.motion.rotation = state.motion_rotation;
    remote.motion.allow_strafe = (state.motion_flags & 1) != 0;
    remote.motion.jumping = (state.motion_flags & 2) != 0;
    remote.mask = state.mask;
    std::memcpy(remote.params, state.params.data(), sizeof(remote.params));
    remote.animations = *clips;
    {
      std::lock_guard<std::shared_mutex> lock(motion_mutex);
      remote_inputs.assign(npc.skeleton, remote);
    }
    if (remote.valid && npc.animated) {
      original_movement_request(npc.animated, &remote.motion);
    }
    if (!npc.skeleton) {
      continue;
    }
    for (unsigned layer = 0; layer < 16; ++layer) {
      const auto& clip = (*clips)[layer];
      auto& applied = npc.animation_sequences[layer];
      if (!clip.sequence || clip.sequence <= applied) {
        continue;
      }
      replaying_animation = true;
      if (!applied || clip.clip < 0) {
        original_clear_animation(npc.skeleton, layer);
      }
      const auto accepted =
          clip.clip < 0 ? 1 : original_start_animation(npc.skeleton, clip.clip, &clip.params);
      replaying_animation = false;
      if (accepted) {
        applied = clip.sequence;
      }
      // Test hook: each clip the replica plays of its simulator's (the gardener).
      static const bool trace = GetEnvironmentVariableW(L"KCDS_TEST_REPLICA_TRACE", nullptr, 0) > 0;
      if (trace) {
        log("NPC replica clip npc=%u clip=%d layer=%u sequence=%llu accepted=%u", id, clip.clip,
            layer, static_cast<unsigned long long>(clip.sequence), unsigned(accepted != 0));
      }
    }
    for (unsigned i = 0; i < 17; ++i) {
      if (state.mask & (1u << i)) {
        original_motion_param(npc.skeleton, int(i), state.params[i]);
      }
    }
  }
  {
    const SlowNpcWatch watch("collect", 0);
    collect_npc_bodies(entities);
  }
  if (measure_combat) {
    report_npc_combat_metrics(now, combat_counts);
  }
}

template void retire_native_npcs<MirrorEntities>(const MirrorEntities&, void*);

template void tick_native_npcs<MirrorEntities>(const MirrorEntities&, void*, void*, const float*,
                                               double);

const std::vector<NpcBody>& npc_bodies() {
  return npc_body_list;
}

void* server_npc_actor(std::uint32_t npc) {
  const auto found = native_npcs.find(core::EntityId(npc));
  return found != native_npcs.end() ? found->second.actor : nullptr;
}

}  // namespace kcds::game_client
