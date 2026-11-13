#include "dice_tick.hpp"

#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../level/level_edits.hpp"
#include "../npcs/native_npcs.hpp"
#include "../npcs/npc_walk.hpp"
#include "../props/pose_math.hpp"
#include "../props/prop_entity.hpp"
#include "../replicas/remote.hpp"
#include "../testing/test_hooks.hpp"
#include "../seats/seat_reservations.hpp"
#include "dice_badges.hpp"
#include "dice_sit.hpp"
#include "dice_table.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "kcd2s/integration/dice_view.hpp"
#include "kcd2s/integration/level_edits.hpp"

namespace kcds::game_client {
namespace {

namespace messages = kcd2s::integration;
namespace dice = kcds::runtime::dice;

// A level EntityGuid (16 hex digits as the level files write it) as the entity system keys it.
std::uint64_t entity_key(std::uint64_t guid) {
  char digits[17] = {};
  std::snprintf(digits, sizeof(digits), "%016llx", static_cast<unsigned long long>(guid));
  std::uint64_t key = 0;
  messages::parse_level_edit_guid(digits, key);
  return key;
}

// A match waiting for its player to sit down (dice_sit.hpp): its start and the server's states
// that came after it, played once the player sits.
struct Seating {
  messages::DiceState start;
  std::uint64_t asked_at = 0;
  std::uint64_t seated_at = 0;
  std::vector<messages::DiceState> held;
  // The server's NPC opponent: whether it was asked to sit, and when it was asked.
  bool npc_asked = false;
  std::uint64_t npc_asked_at = 0;
};
std::optional<Seating> seating;
// The running match's opponent is the server's NPC: this game tells its turn too.
bool npc_opponent_match = false;
// The sit-down after the seat is taken (the game's sit-down clips last 2.2 to 3 s), and the
// longest a match waits for it.
constexpr std::uint64_t kSitDownMilliseconds = 3200;
constexpr std::uint64_t kSeatingLimitMilliseconds = 10000;
// The table the server last sat the local player down at to wait for a match (DiceState Sit),
// when that was asked and when the player came to hold the seat (0 for not yet). The game reserves
// the seat when the walk to it starts, so a match there still waits for the sit-down after it.
struct Waiting {
  std::uint64_t behavior = 0;
  std::uint64_t asked_at = 0;
  std::uint64_t held_at = 0;
};
std::optional<Waiting> waiting_at_table;

// The server sits the local player down at a table's chair to wait for another player. False when
// this game has no such table (one of a quest layer the game shows only during its quest): the
// server is told, and leaves it out.
bool sit_and_wait(const messages::DiceState& state) {
  const std::uint64_t table_key = entity_key(state.behavior);
  const auto found = kcds::runtime::level::entities_by_guid({table_key});
  const auto table = found.find(table_key);
  if (table == found.end()) {
    log("DICE sit: the table %016llx is not in this world",
        static_cast<unsigned long long>(state.behavior));
    return false;
  }
  if (!kcds::runtime::dice::sit::sit_down(table->second, state.seat)) {
    log("DICE sit: the sit-down at seat %u could not go", unsigned(state.seat));
    return true;
  }
  Waiting waiting;
  waiting.behavior = state.behavior;
  waiting.asked_at = GetTickCount64();
  waiting_at_table = waiting;
  log("DICE sit: the player sits down at seat %u and waits", unsigned(state.seat));
  return true;
}

// The table behaviour of each running match by its id, from its start to its end, so the local
// player can get up from that table when the match ends.
std::map<std::uint32_t, std::uint64_t> match_tables;
// The table of a match that has ended: a win the game reaches itself still closes on its own after
// the server's end, so the player gets up when the minigame has gone (tick_dice).
std::optional<std::uint64_t> table_to_leave;
// The NPC opponents this game sat down, by match (opponent_ready), and the one of a match that has
// ended with its table: it gets up the same way as the player, or it stayed in its chair.
std::map<std::uint32_t, std::uint32_t> npcs_seated_here;
struct NpcToLeave {
  std::uint64_t table = 0;
  std::uint32_t npc = 0;
};
std::optional<NpcToLeave> npc_to_leave;

// After a match ends the local player gets up from the table's chair, as the game's own end does
// (the table's instantlyLeaveDiceTable interrupt). A match the table is not known for leaves them
// seated, as before.
void leave_ended_table() {
  if (!table_to_leave || dice::at_table()) {
    return;
  }
  // The game refuses the interrupt while the minigame is still closing: asked again for a while.
  constexpr std::uint64_t kRetryMs = 500;
  constexpr int kMostTries = 20;
  static int tries = 0;
  static std::uint64_t asked_at = 0;
  const std::uint64_t now = GetTickCount64();
  if (tries > 0 && now - asked_at < kRetryMs) {
    return;
  }
  asked_at = now;
  const std::uint64_t table_key = entity_key(*table_to_leave);
  const auto found = kcds::runtime::level::entities_by_guid({table_key});
  const auto table = found.find(table_key);
  if (table == found.end()) {
    table_to_leave.reset();
    tries = 0;
    log("DICE the ended match's table is not in this world, the player stays seated");
    return;
  }
  if (kcds::runtime::dice::sit::leave_table(table->second)) {
    log("DICE the player gets up from the ended match's table (try %d)", tries + 1);
    table_to_leave.reset();
    tries = 0;
    return;
  }
  if (++tries >= kMostTries) {
    log("DICE the player could not get up from the ended match's table");
    table_to_leave.reset();
    tries = 0;
  }
}

// The opponent's body in this game: the other player's replica, or the server's NPC;
// 0 when it is not here. An NPC's first body may be replaced as soon as its simulator is known,
// which would take it from under the running minigame (the match ended or the client stopped,
// live), so the match waits for the lasting one.
std::uint32_t opponent_body(std::uint32_t network_id) {
  const auto replica = remotes.find(network_id);
  if (replica != remotes.end()) {
    return replica->second.engine_id;
  }
  if (!npc_body_settled(network_id)) {
    return 0;
  }
  void* actor = nullptr;
  return engine_body_of(network_id, actor);
}

// After a match against an NPC this game sat down, the NPC gets up from its chair as the player
// does (leave_ended_table). The server takes its own stand-in away: a body that is gone ends it.
void npc_leaves_ended_table() {
  if (!npc_to_leave || dice::at_table()) {
    return;
  }
  constexpr std::uint64_t kRetryMs = 500;
  constexpr int kMostTries = 20;
  static int tries = 0;
  static std::uint64_t asked_at = 0;
  const std::uint64_t now = GetTickCount64();
  if (tries > 0 && now - asked_at < kRetryMs) {
    return;
  }
  asked_at = now;
  const std::uint64_t table_key = entity_key(npc_to_leave->table);
  const auto found = kcds::runtime::level::entities_by_guid({table_key});
  const auto table = found.find(table_key);
  const std::uint32_t body_id = opponent_body(npc_to_leave->npc);
  void* body = body_id != 0 ? kcds::runtime::props::entity::by_id(body_id) : nullptr;
  void* agent = body ? kcds::runtime::npcs::walk::agent_of(body) : nullptr;
  if (table == found.end() || !agent) {
    npc_to_leave.reset();
    tries = 0;
    return;
  }
  if (kcds::runtime::dice::sit::npc_leave_table(table->second, agent)) {
    log("DICE the NPC gets up from the ended match's table (try %d)", tries + 1);
    npc_to_leave.reset();
    tries = 0;
    return;
  }
  if (++tries >= kMostTries) {
    log("DICE the NPC could not get up from the ended match's table");
    npc_to_leave.reset();
    tries = 0;
  }
}

// The match's table behavior and the opponent's body in this game; false when either is not here.
bool start_now(const messages::DiceState& state) {
  const std::uint64_t table_key = entity_key(state.behavior);
  const auto found = kcds::runtime::level::entities_by_guid({table_key});
  const auto table = found.find(table_key);
  const std::uint32_t opponent = opponent_body(state.opponent);
  if (table == found.end() || opponent == 0) {
    log("DICE match %u: %s is not in this world", state.match,
        table == found.end() ? "the table" : "the other player");
    return false;
  }
  npc_opponent_match = remotes.find(state.opponent) == remotes.end();
  const auto npc_body = native_npcs.find(state.opponent);
  dice::set_seated_npc(
      npc_opponent_match && npc_body != native_npcs.end() ? npc_body->second.skeleton : nullptr);
  dice::Start start;
  start.match = state.match;
  start.table = kcds::runtime::props::entity::id_of(table->second);
  start.opponent = opponent;
  start.seat = state.seat;
  start.first = state.first;
  start.target = state.target;
  start.own_name = state.own_name;
  start.opponent_name = state.opponent_name;
  start.own_dice = state.own_dice;
  start.opponent_dice = state.opponent_dice;
  start.own_sides = state.own_sides;
  start.opponent_sides = state.opponent_sides;
  start.own_badge = state.own_badge.present() ? int(state.own_badge.effect) : -1;
  start.own_badge_tier = int(state.own_badge.tier);
  start.opponent_badge = state.opponent_badge.present() ? int(state.opponent_badge.effect) : -1;
  dice::badges::prepare(state.match, start.opponent, state.own_badge, state.opponent_badge);
  return dice::start(start);
}

// Whether the match's opponent is ready for the minigame. The game's minigame puts its two players
// in their seats when it starts, which showed as an NPC snapping into its chair: the
// server's NPC sits down the game's way first. Another player sits in their own game. An NPC
// another player's game runs is not this game's to seat.
bool opponent_ready(Seating& waiting) {
  const std::uint32_t network_id = waiting.start.opponent;
  const std::uint32_t body_id = opponent_body(network_id);
  if (body_id == 0) {
    return false;
  }
  if (remotes.find(network_id) != remotes.end()) {
    return true;
  }
  void* body = kcds::runtime::props::entity::by_id(body_id);
  void* agent = body ? kcds::runtime::npcs::walk::agent_of(body) : nullptr;
  if (!agent) {
    return true;
  }
  const std::uint64_t now = GetTickCount64();
  if (!waiting.npc_asked) {
    const std::uint64_t table_key = entity_key(waiting.start.behavior);
    const auto found = kcds::runtime::level::entities_by_guid({table_key});
    const auto table = found.find(table_key);
    const std::uint8_t seat = std::uint8_t(1 - waiting.start.seat);
    if (table == found.end() || !kcds::runtime::dice::sit::seat_npc(table->second, agent, seat)) {
      log("DICE match %u: the NPC could not sit down", waiting.start.match);
      return true;
    }
    waiting.npc_asked = true;
    waiting.npc_asked_at = now;
    npcs_seated_here[waiting.start.match] = network_id;
    log("DICE match %u: the NPC sits down on chair %u", waiting.start.match, unsigned(seat));
    return false;
  }
  const std::uint64_t taken = kcds::runtime::seats::seat_taken_at(body_id);
  return taken >= waiting.npc_asked_at && now - taken >= kSitDownMilliseconds;
}

// A match starts once its player sat down at the table the game's way, or at once when the
// sit-down cannot be asked for.
bool start_match(const messages::DiceState& state) {
  const std::uint64_t table_key = entity_key(state.behavior);
  const auto found = kcds::runtime::level::entities_by_guid({table_key});
  const auto table = found.find(table_key);
  if (!seating && waiting_at_table && waiting_at_table->behavior == state.behavior) {
    // The player sat down (or is sitting down) there already: the match waits for the sit-down.
    Seating waiting;
    waiting.start = state;
    waiting.asked_at = GetTickCount64();
    waiting.seated_at = waiting_at_table->held_at;
    seating = std::move(waiting);
    waiting_at_table.reset();
    log("DICE match %u: the player sits at the table already", state.match);
    return true;
  }
  waiting_at_table.reset();
  if (seating || table == found.end()) {
    return start_now(state);
  }
  // The opponent's body need not be here yet: an NPC the server stood at the table for the match
  // comes a moment after the start. The match waits for it as for the sit-down.
  const bool sitting = kcds::runtime::dice::sit::sit_down(table->second, state.seat);
  const bool other_player = remotes.find(state.opponent) != remotes.end();
  if (!sitting && other_player) {
    return start_now(state);
  }
  Seating waiting;
  waiting.start = state;
  waiting.asked_at = GetTickCount64();
  if (!sitting) {
    // Seated already (or no sit-down to ask for): only the opponent is waited for.
    waiting.seated_at = waiting.asked_at - kSitDownMilliseconds;
  }
  seating = std::move(waiting);
  return true;
}

// While the local player waits at a table, when they came to hold its seat; the wait ends when
// they stand up (the seat no longer held).
void keep_waiting() {
  if (!waiting_at_table) {
    return;
  }
  const bool holds = !kcds::runtime::seats::local_seats().empty();
  const std::uint64_t now = GetTickCount64();
  if (waiting_at_table->held_at == 0) {
    if (holds) {
      waiting_at_table->held_at = now;
    } else if (now - waiting_at_table->asked_at >= kSeatingLimitMilliseconds) {
      log("DICE sit: the player did not sit down in time");
      waiting_at_table.reset();
    }
    return;
  }
  if (!holds) {
    log("DICE sit: the player stood up");
    waiting_at_table.reset();
  }
}

// The match waiting for its player's sit-down: the states that came for now held, and the match
// started once the player holds the seat and sat down (or the wait ran out). The states to play
// this frame.
std::vector<messages::DiceState> take_seated(std::vector<messages::DiceState> states,
                                             std::vector<messages::DiceMove>& failed) {
  if (!seating) {
    return states;
  }
  Seating& waiting = *seating;
  for (auto& state : states) {
    waiting.held.push_back(std::move(state));
  }
  for (const auto& state : waiting.held) {
    if (state.match == waiting.start.match && state.kind == messages::DiceState::Kind::End) {
      log("DICE match %u ended while its player sat down", state.match);
      std::vector<messages::DiceState> rest = std::move(waiting.held);
      seating.reset();
      return rest;
    }
  }
  const std::uint64_t now = GetTickCount64();
  if (waiting.seated_at == 0 && !kcds::runtime::seats::local_seats().empty()) {
    waiting.seated_at = now;
  }
  const bool sat = waiting.seated_at != 0 && now - waiting.seated_at >= kSitDownMilliseconds;
  const bool opponent_here = opponent_ready(waiting);
  if ((!sat || !opponent_here) && now - waiting.asked_at < kSeatingLimitMilliseconds) {
    return {};
  }
  log("DICE match %u: the player %s, the minigame starts", waiting.start.match,
      sat ? "sat down" : "did not sit down in time");
  if (!start_now(waiting.start)) {
    failed.push_back(messages::DiceMove{waiting.start.match, messages::DiceMove::Kind::Failed, 0});
  }
  std::vector<messages::DiceState> rest = std::move(waiting.held);
  seating.reset();
  return rest;
}

// A level entity's key in the entity system (CEntity slot 2), and whether it is hidden (its
// flags at + 8, 0x10).
constexpr std::size_t kEntityKeySlot = 2;
constexpr std::size_t kEntityFlags = 8;
constexpr std::uint32_t kEntityHidden = 0x10;
// How often the local player's throw goes to the players watching, at most (milliseconds).
constexpr std::uint64_t kViewMilliseconds = 50;
// A watched thing glides from where it was shown to where the thrower's game has it over the time
// between the two views (at least and at most these, in milliseconds); it jumps when it was hidden
// or moved farther than kWatchJumpMeters (the dice wait at the world's origin in the cup).
constexpr std::uint64_t kWatchGlideMinMilliseconds = 30;
constexpr std::uint64_t kWatchGlideMaxMilliseconds = 250;
constexpr float kWatchJumpMeters = 1.5f;
// The dice of a throw are entities each game's minigame spawns for itself (another game does not
// have them): they go by their place in the throw (this bit and the index) with their mesh file
// and scale, and a watching game shows dice of its own for them. The cup is the table's own, by its
// key. A die whose mesh is not known shows as the game's basic die.
constexpr std::uint64_t kSpawnedDie = 0x8000000000000000ull;
const char* const kDieModel =
    "objects/manmade/task_specific_props/entertainment/games/dice/dice_basic.cgf";

std::uint64_t key_of(void* entity) {
  using Key = std::uint64_t(__fastcall*)(void* entity);
  return (*static_cast<Key**>(entity))[kEntityKeySlot](entity);
}

bool hidden(void* entity) {
  std::uint32_t flags = 0;
  std::memcpy(&flags, static_cast<std::uint8_t*>(entity) + kEntityFlags, sizeof(flags));
  return (flags & kEntityHidden) != 0;
}

std::vector<messages::DiceThing> told_things;
std::uint64_t told_at = 0;
// Whether the local player's game shows another player's throw (a watch is on).
bool watching = false;

// The local player's throw: the table's things as the game has them, to the server when they
// changed.
void send_own_view() {
  std::uint32_t match = 0;
  std::uint32_t cup = 0;
  std::vector<std::uint32_t> ids = dice::own_table_things(match, cup, npc_opponent_match);
  // After the local player's turn passed, only until the other player's throw is watched here.
  if ((ids.empty() && !cup) || (!dice::own_turn_now() && !npc_opponent_match && watching)) {
    told_things.clear();
    return;
  }
  const std::uint64_t now = GetTickCount64();
  if (now - told_at < kViewMilliseconds) {
    return;
  }
  messages::DiceView view;
  view.match = match;
  if (cup) {
    ids.push_back(cup);
  }
  for (std::size_t index = 0; index < ids.size(); ++index) {
    void* entity = kcds::runtime::props::entity::by_id(ids[index]);
    if (!entity) {
      continue;
    }
    messages::DiceThing thing;
    const bool die = !cup || index + 1 < ids.size();
    thing.key = die ? kSpawnedDie | index : key_of(entity);
    kcds::runtime::props::entity::world_pose(entity, thing.position.data(), thing.rotation.data());
    thing.shown = !hidden(entity);
    if (die) {
      thing.model = kcds::runtime::props::entity::mesh_file(entity, messages::kMaxDiceModel);
      thing.material = kcds::runtime::props::entity::material_name(entity, messages::kMaxDiceModel);
      const float scale = kcds::runtime::props::entity::world_scale(entity);
      if (std::isfinite(scale) && scale > 0.0f && scale <= messages::kMaxDiceScale) {
        thing.scale = scale;
      }
    }
    view.things.push_back(thing);
  }
  if (view.things.empty() || view.things == told_things) {
    return;
  }
  const bool first = told_things.empty();
  told_at = now;
  told_things = view.things;
  std::lock_guard<std::mutex> lock(network_mutex);
  const bool sent = client->peer().send_dice_view(view);
  if (first) {
    const auto& thing = view.things.front();
    log("DICE match %u: the local player's throw to the players watching: %s (%zu things, "
        "the first %016llx at %.2f %.2f %.2f turned %.3f %.3f %.3f %.3f, %s in %s scaled %.3f)",
        match, sent ? "sent" : "not sent", view.things.size(),
        static_cast<unsigned long long>(thing.key), thing.position[0], thing.position[1],
        thing.position[2], thing.rotation[0], thing.rotation[1], thing.rotation[2],
        thing.rotation[3], thing.model.c_str(), thing.material.c_str(), thing.scale);
  }
}

// A thing of a table watched: the entity, and how the level had it before (a die of the
// watching game's own goes away at the end instead).
struct Watched {
  void* entity = nullptr;
  float position[3] = {};
  float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  bool hidden = false;
  std::string spawned;
  // The scale a die of the watching game's own was last given (the thrower's dice shrink as they
  // leave the cup: 1 in it, 0.462 on the board, live), and the mesh it shows.
  float scale = 0.0f;
  std::string model;
  // A die of the game's own the view moves (the other player's in a match): its scale before.
  float own_scale = 0.0f;
  // Its glide: from where it was shown to the thrower's latest place, over span milliseconds from
  // glide_at; viewed_at the last view, shown whether it showed then.
  float from_position[3] = {};
  float from_rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  float to_position[3] = {};
  float to_rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  std::uint64_t glide_at = 0;
  std::uint64_t span = 0;
  std::uint64_t viewed_at = 0;
  bool shown = false;
};

// Where a watched thing is shown at `now` along its glide: the position in a straight line, the
// turn the shorter way round.
void glide_pose(const Watched& thing, std::uint64_t now, float position[3], float rotation[4]) {
  float t = 1.0f;
  if (thing.span > 0 && now < thing.glide_at + thing.span) {
    t = float(now - thing.glide_at) / float(thing.span);
  }
  for (int i = 0; i < 3; ++i) {
    position[i] = thing.from_position[i] + (thing.to_position[i] - thing.from_position[i]) * t;
  }
  float dot = 0.0f;
  for (int i = 0; i < 4; ++i) {
    dot += thing.from_rotation[i] * thing.to_rotation[i];
  }
  const float side = dot < 0.0f ? -1.0f : 1.0f;
  for (int i = 0; i < 4; ++i) {
    rotation[i] =
        thing.from_rotation[i] + (side * thing.to_rotation[i] - thing.from_rotation[i]) * t;
  }
  kcds::runtime::pose_math::normalize(rotation);
}

// A new place for a watched thing from the thrower's game: it glides there from where it is shown
// now, or jumps.
void glide_to(Watched& thing, const messages::DiceThing& view, std::uint64_t now) {
  float position[3] = {};
  float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  glide_pose(thing, now, position, rotation);
  float distance = 0.0f;
  for (int i = 0; i < 3; ++i) {
    const float d = view.position[i] - position[i];
    distance += d * d;
  }
  const bool jump = thing.viewed_at == 0 || !thing.shown || !view.shown ||
                    distance > kWatchJumpMeters * kWatchJumpMeters;
  std::uint64_t span = now - thing.viewed_at;
  if (span < kWatchGlideMinMilliseconds) {
    span = kWatchGlideMinMilliseconds;
  }
  if (span > kWatchGlideMaxMilliseconds) {
    span = kWatchGlideMaxMilliseconds;
  }
  for (int i = 0; i < 3; ++i) {
    thing.from_position[i] = jump ? view.position[i] : position[i];
    thing.to_position[i] = view.position[i];
  }
  for (int i = 0; i < 4; ++i) {
    thing.from_rotation[i] = jump ? view.rotation[i] : rotation[i];
    thing.to_rotation[i] = view.rotation[i];
  }
  thing.glide_at = now;
  thing.span = jump ? 0 : span;
  thing.viewed_at = now;
  thing.shown = view.shown;
}

// key -> the thing watched
std::map<std::uint64_t, Watched> watched;

// Every frame: the watched things where their glides have them.
void glide_watched() {
  const std::uint64_t now = GetTickCount64();
  for (const auto& [key, thing] : watched) {
    if (thing.viewed_at == 0) {
      continue;
    }
    float position[3] = {};
    float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    glide_pose(thing, now, position, rotation);
    kcds::runtime::props::entity::place_rotated(thing.entity, position, rotation);
    // A die of the game's own (the other player's in a match): the game hides and shrinks it
    // again once its own throw animation is over, so it is shown and sized every frame (they
    // vanished from the table after the throw).
    if (thing.own_scale > 0.0f) {
      kcds::runtime::props::entity::hide(thing.entity, !thing.shown);
      if (thing.scale > 0.0f && std::fabs(kcds::runtime::props::entity::world_scale(thing.entity) -
                                          thing.scale) > 0.01f) {
        const float scale[3] = {thing.scale, thing.scale, thing.scale};
        kcds::runtime::props::entity::set_scale(thing.entity, scale);
      }
    }
  }
}

// The other player's watch that ended is put back when the local player's turn begins in the game,
// at the latest at this time (0: none waits).
constexpr std::uint64_t kPutBackWaitMilliseconds = 6000;
std::uint64_t put_back_at = 0;
// The match whose watch ended last, and until when views of it may still come.
std::uint32_t ended_match = 0;
std::uint64_t ended_until = 0;

void put_back() {
  put_back_at = 0;
  watching = false;
  // The match's cup goes to its place on the table as the game has it (where it was when the watch
  // began could be in the air, in the local player's hand on its way back).
  float cup_place[3] = {};
  float cup_turn[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  void* cup = dice::cup_rest(cup_place, cup_turn);
  for (const auto& [key, thing] : watched) {
    if (!thing.spawned.empty()) {
      kcds::runtime::props::entity::remove(thing.spawned);
      continue;
    }
    if (cup && thing.entity == cup) {
      kcds::runtime::props::entity::place_rotated(thing.entity, cup_place, cup_turn);
    } else {
      kcds::runtime::props::entity::place_rotated(thing.entity, thing.position, thing.rotation);
    }
    kcds::runtime::props::entity::hide(thing.entity, thing.hidden);
    if (thing.own_scale > 0.0f) {
      const float scale[3] = {thing.own_scale, thing.own_scale, thing.own_scale};
      kcds::runtime::props::entity::set_scale(thing.entity, scale);
    }
  }
  watched.clear();
}

// A match nearby as the local player watches it: the table's things where the thrower's game
// has them; at the end as the level had them.
void show_watch(const messages::DiceWatch& watch) {
  if (watch.ended) {
    // The other player of a match: the server passes their turn as it deals it (a bust) or as they
    // pass, while their game still throws, scores and passes on the board, and this game shows
    // it; their dice stay until the local player's turn begins here (they vanished from under the
    // camera, or a bust's dice never came).
    if (dice::at_table() && !dice::own_turn_now()) {
      ended_match = watch.match;
      ended_until = GetTickCount64() + kPutBackWaitMilliseconds;
      put_back_at = ended_until;
      return;
    }
    put_back();
    return;
  }
  // The passing player's game goes on telling the board for a while (the kept dice going aside):
  // shown while their dice wait to be put back, left out once they are.
  if (watch.match == ended_match && GetTickCount64() < ended_until && put_back_at == 0) {
    return;
  }
  watching = true;
  std::vector<std::uint64_t> missing;
  for (const auto& thing : watch.things) {
    if (watched.count(thing.key) != 0) {
      continue;
    }
    if ((thing.key & kSpawnedDie) == 0) {
      missing.push_back(thing.key);
      continue;
    }
    // The other player of the match: their own game's die of that place in the throw goes where
    // the thrower's game has it, so the game's board camera and the selection show it (copies
    // beside the game's dice, heaped on the board, left the camera looking at the heap).
    if (const std::uint32_t own = dice::opponent_die(thing.key & ~kSpawnedDie)) {
      if (void* entity = kcds::runtime::props::entity::by_id(own)) {
        Watched die;
        die.entity = entity;
        kcds::runtime::props::entity::world_pose(entity, die.position, die.rotation);
        die.hidden = hidden(entity);
        // The game keeps the other player's dice shrunk away while it throws none of them: they
        // take the thrower's scale with their places.
        die.own_scale = kcds::runtime::props::entity::world_scale(entity);
        log("DICE match %u watched: die %llu is this game's %u at %.2f,%.2f,%.2f scaled %.3f%s",
            watch.match, static_cast<unsigned long long>(thing.key & ~kSpawnedDie), own,
            die.position[0], die.position[1], die.position[2], die.own_scale,
            die.hidden ? ", hidden" : "");
        watched[thing.key] = die;
        continue;
      }
    }
    Watched die;
    die.spawned = "kcds_dice_watch_" + std::to_string(thing.key & ~kSpawnedDie);
    const std::string model = thing.model.empty() ? std::string(kDieModel) : thing.model;
    die.entity = kcds::runtime::props::entity::spawn_mesh(die.spawned, thing.position.data(), model,
                                                          thing.material);
    log("DICE match %u watched: die %llu shows %s in %s scaled %.3f: %s", watch.match,
        static_cast<unsigned long long>(thing.key & ~kSpawnedDie), model.c_str(),
        thing.material.c_str(), thing.scale, die.entity ? "spawned" : "not spawned");
    if (die.entity) {
      die.model = model;
      watched[thing.key] = die;
    }
  }
  if (!missing.empty()) {
    const auto found = kcds::runtime::level::entities_by_guid(missing);
    log("DICE match %u watched: %zu of %zu of the table's things found here", watch.match,
        found.size(), missing.size());
    for (const auto& [key, entity] : found) {
      Watched thing;
      thing.entity = entity;
      kcds::runtime::props::entity::world_pose(entity, thing.position, thing.rotation);
      thing.hidden = hidden(entity);
      watched[key] = thing;
    }
  }
  for (const auto& thing : watch.things) {
    const auto found = watched.find(thing.key);
    if (found == watched.end()) {
      continue;
    }
    // A die of the game's kinds: the thrower's game put its mesh on after the first view went out.
    if (!found->second.spawned.empty() && !thing.model.empty() &&
        _stricmp(found->second.model.c_str(), thing.model.c_str()) != 0) {
      kcds::runtime::props::entity::show_mesh(found->second.entity, thing.model);
      found->second.model = thing.model;
    }
    const bool die = (thing.key & kSpawnedDie) != 0;
    if (die && thing.scale > 0.0f && found->second.scale != thing.scale) {
      const float scale[3] = {thing.scale, thing.scale, thing.scale};
      kcds::runtime::props::entity::set_scale(found->second.entity, scale);
      found->second.scale = thing.scale;
    }
    glide_to(found->second, thing, GetTickCount64());
    kcds::runtime::props::entity::hide(found->second.entity, !thing.shown);
  }
  glide_watched();
}

// Every dice table offers "Play dice" to the local player (DiceInteractor's diceAvailable, which
// the game's AI turns on only while an NPC waits there to play) while the server says a resource
// answers a table's use (DICE_TABLES): using one tells the server (TableUse), whose
// resources decide what follows (diceTableUse). Without one, no table offers it. Kept so every
// five seconds, and at once when the server's word changes. A server that never says keeps it
// offered, as before.
constexpr std::uint64_t kOfferTablesMilliseconds = 5000;
bool tables_offered = true;

void offer_tables(bool changed) {
  static std::uint64_t offered_at = 0;
  const std::uint64_t now = GetTickCount64();
  if (!changed && now - offered_at < kOfferTablesMilliseconds) {
    return;
  }
  offered_at = now;
  kcds::runtime::props::entity::each_of_class("DiceInteractor", [](void* table) {
    kcds::runtime::props::entity::set_script_flag(table, "diceAvailable", tables_offered);
  });
}

}  // namespace

// The server's states of the local player's match go to the game's table; the local player's
// moves, and a match the game could not start (failed), go to the server.
void tick_dice() {
  std::vector<messages::DiceState> states;
  std::vector<messages::DiceWatch> watches;
  std::optional<messages::DiceTables> tables;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    for (const auto& move : dice::take_moves()) {
      client->peer().send_dice_move(move);
      // The local player throws: the other player's next turn is watched anew.
      if (move.kind == messages::DiceMove::Kind::Throw) {
        ended_until = 0;
      }
    }
    states = client->peer().take_dice_states();
    watches = client->peer().take_dice_watches();
    tables = client->peer().take_dice_tables();
  }
  const bool tables_changed = tables && tables->offered != tables_offered;
  if (tables_changed) {
    tables_offered = tables->offered;
    log("DICE tables %s: %s", tables_offered ? "offered" : "not offered",
        tables_offered ? "a resource answers a table's use" : "no resource answers a table's use");
  }
  for (const auto& watch : watches) {
    show_watch(watch);
  }
  send_own_view();
  offer_tables(tables_changed);
  std::vector<messages::DiceMove> failed;
  keep_waiting();
  states = take_seated(std::move(states), failed);
  for (const auto& state : states) {
    // After a start that waits for its player's sit-down, the rest waits with it.
    if (seating) {
      seating->held.push_back(state);
      continue;
    }
    if (state.kind != messages::DiceState::Kind::Roll) {
      log("DICE match %u: the server's %s", state.match,
          state.kind == messages::DiceState::Kind::Start  ? "start"
          : state.kind == messages::DiceState::Kind::Move ? "move of the other player"
          : state.kind == messages::DiceState::Kind::End  ? "end"
          : state.kind == messages::DiceState::Kind::Sit  ? "sit-down"
                                                          : "names");
    }
    switch (state.kind) {
      case messages::DiceState::Kind::Start:
        match_tables[state.match] = state.behavior;
        if (!start_match(state)) {
          failed.push_back(messages::DiceMove{state.match, messages::DiceMove::Kind::Failed, 0});
        }
        break;
      case messages::DiceState::Kind::Roll:
        dice::roll(state.match, state.mine, state.faces, state.rolled);
        break;
      case messages::DiceState::Kind::Move:
        if (state.move == messages::DiceState::Move::Select) {
          dice::opponent_select(state.match, state.held);
        } else {
          dice::opponent_move(state.match, state.move, state.held);
        }
        break;
      case messages::DiceState::Kind::End:
        dice::end(state.match, state.reason, state.result);
        dice::badges::forget(state.match);
        if (const auto known = match_tables.find(state.match); known != match_tables.end()) {
          table_to_leave = known->second;
          if (const auto npc = npcs_seated_here.find(state.match); npc != npcs_seated_here.end()) {
            npc_to_leave = NpcToLeave{known->second, npc->second};
          }
          match_tables.erase(known);
        }
        npcs_seated_here.erase(state.match);
        break;
      case messages::DiceState::Kind::Names:
        dice::names(state.match, state.own_name, state.opponent_name);
        break;
      case messages::DiceState::Kind::Sit:
        if (!sit_and_wait(state)) {
          failed.push_back(messages::DiceMove{0, messages::DiceMove::Kind::TableMissing, 0});
        }
        break;
    }
  }
  if (!failed.empty()) {
    std::lock_guard<std::mutex> lock(network_mutex);
    for (const auto& move : failed) {
      client->peer().send_dice_move(move);
    }
  }
  if (signaled(dice_autoplay_event)) {
    dice::set_autoplay(!dice::autoplay_on());
  }
  dice::tick();
  // The watched things go where the thrower's game has them after the game's tick: selecting the
  // other player's dice there puts them back where the game had them (they blinked for a frame).
  glide_watched();
  if (put_back_at != 0 &&
      (dice::own_turn_now() || !dice::at_table() || GetTickCount64() >= put_back_at)) {
    put_back();
  }
  leave_ended_table();
  npc_leaves_ended_table();
}

}  // namespace kcds::game_client
