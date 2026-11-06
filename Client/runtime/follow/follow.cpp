#include "follow.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "../action_hints/resource_action_hints.hpp"
#include "../input/cuffs.hpp"
#include "../props/prop_entity.hpp"
#include "kcd2s/integration/script_channel.hpp"

// After the headers above: MinHook brings in windows.h and its min and max macros.
#include "MinHook.h"

namespace kcds::runtime::follow {
namespace {

namespace sdk = kcd2s::sdk;

// The actor's action actor and its slot that finds the running action of a type (as the carry).
constexpr std::size_t kActorActions = 640;
constexpr std::size_t kActionsFind = 64 / 8;
// C_ActorActionFollow's type (its slot 14).
constexpr std::uint8_t kFollowAction = 30;
constexpr std::size_t kReleaseReference = 16 / 8;
// The actor's human (+ 0x990) keeps its restrictions at + 0x370; restriction 26 forbids following
// (as asks).
constexpr std::size_t kActorHuman = 0x990;
constexpr std::size_t kHumanRestrictions = 0x370;
constexpr std::size_t kRestrictionsIs = 8 / 8;
constexpr std::uint8_t kFollowRestriction = 26;
// C_ActorAction's finish result for an action ended from outside.
constexpr std::uint32_t kFinished = 2;
// How far a follow may start from its target (metres), and how long a started one may take to
// show up as the running action before it counts as failed (seconds).
constexpr float kStartDistance = 20.0f;
constexpr double kStartGraceSeconds = 3.0;
// A move of the local player this far within one frame is a teleport (metres).
constexpr float kTeleportDistance = 5.0f;

using Start = void(__fastcall*)(void* actions, const std::uint32_t* entity);
using Finish = void(__fastcall*)(void* action, std::uint32_t result);
using FindAction = void(__fastcall*)(void* actions, void** action, std::uint8_t type);
using Restricted = bool(__fastcall*)(void* restrictions, std::uint8_t restriction);
using Release = void(__fastcall*)(void* object);
using MayFollow = bool(__fastcall*)(void* manager, std::uint32_t entity);
using MayKeepFollowing = bool(__fastcall*)(void* manager);

struct Request {
  std::int64_t id = 0;
  sdk::Value body;
};

struct Reply {
  std::int64_t id = 0;
  bool value = false;
};

struct Event {
  std::string event;
  std::string reason;
};

Natives natives;
std::mutex queue_mutex;
std::vector<Request> requests;
std::vector<Reply> replies;
std::vector<Event> events;
// The follow this client started: its target, whether the action came up, when it was asked.
std::uint32_t target = 0;
bool pending = false;
bool active = false;
bool stopping = false;
// Why this client is ending its follow: "script" (Follow.stop) or one of own_stop's.
const char* stop_reason = "script";
// An end this client asks for itself, which the engine frame carries out: "input" (a movement
// press), "mountChanged" (the player mounted or got off) or "teleported"; null for none.
const char* own_stop = nullptr;
// The player used the game's own "Stop following" key (chat_focus_follow_init, L Alt), which ends
// the action in the game's input handling: its end is the player's input too.
bool stop_key_used = false;
// Follow.start's options: the game's "Stop following" prompt shows, and the player
// may end the follow with that key or a movement press.
bool show_hint = true;
bool player_can_stop = true;
// Follow.start's pace 'run': the follow does not hold the player to a walk.
std::atomic<bool> run_pace{false};
// Follow.start's distance: how close the follower keeps, in metres; 0 for the game's.
std::atomic<float> wanted_distance{0.0f};
constexpr float kDistanceMin = 0.5f;
constexpr float kDistanceMax = 10.0f;
// The follower's gap to its target this frame (metres, on the ground), and whether it catches up:
// a follow with a distance jogs from kCatchUpFrom past it and walks again within kCatchUpTo, so a
// walking leader does not leave it behind (both walk alike: a follower that fell back never closed
// the gap). A cuffed player only walks.
std::atomic<float> gap_now{0.0f};
bool catching_up = false;
constexpr float kCatchUpFrom = 1.0f;
constexpr float kCatchUpTo = 0.3f;
// The game's prompt for its stop key.
constexpr char kStopHintAction[] = "chat_focus_follow_init_press";
// Whether the local player sat on a horse when the follow started, and where it was last frame.
bool mounted_now = false;
bool started_mounted = false;
bool has_last_position = false;
float last_position[3] = {};
double asked_at = 0;
MayFollow original_may_follow = nullptr;
MayKeepFollowing original_may_keep_following = nullptr;

template <class Fn>
Fn slot_of(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*static_cast<void***>(object))[index]);
}

template <class T>
T at(const void* object, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

double seconds_now() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

// The actor's running follow action, or null. The find hands out a reference, released here: the
// action actor keeps the action while it runs, and the caller uses it in the same frame only.
void* running_follow(void* actor) {
  void* actions = actor ? at<void*>(actor, kActorActions) : nullptr;
  if (!actions) {
    return nullptr;
  }
  void* action = nullptr;
  slot_of<FindAction>(actions, kActionsFind)(actions, &action, kFollowAction);
  if (action) {
    slot_of<Release>(action, kReleaseReference)(action);
  }
  return action;
}

bool restricted(void* actor) {
  void* human = at<void*>(actor, kActorHuman);
  if (!human) {
    return true;
  }
  void* restrictions = static_cast<std::uint8_t*>(human) + kHumanRestrictions;
  return slot_of<Restricted>(restrictions, kRestrictionsIs)(restrictions, kFollowRestriction);
}

std::uint32_t entity_of(const sdk::Value& request) {
  const sdk::Value* value = kcd2s::integration::script_field(request, "entity");
  double id = 0;
  if (value && value->tag() == sdk::Tag::Int) {
    id = double(value->as_int());
  } else if (value && value->tag() == sdk::Tag::Double) {
    id = value->as_double();
  }
  return id >= 1 && id <= 4294967295.0 ? std::uint32_t(id) : 0;
}

// Whether the local player may start following this entity now.
bool can_start(void* entity_system, void* local_actor, void* local_entity, std::uint32_t entity) {
  if (!natives.start || !natives.get_entity || !local_actor || !local_entity || entity == 0) {
    return false;
  }
  void* body = natives.get_entity(entity_system, entity);
  if (!body || body == local_entity || restricted(local_actor)) {
    return false;
  }
  float from[3];
  float to[3];
  props::entity::world_position(local_entity, from);
  props::entity::world_position(body, to);
  const float dx = to[0] - from[0];
  const float dy = to[1] - from[1];
  const float dz = to[2] - from[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz) <= kStartDistance;
}

void raise(const char* event, const char* reason) {
  std::lock_guard<std::mutex> lock(queue_mutex);
  events.push_back(Event{event, reason});
}

bool stop(void* local_actor, const char* reason) {
  void* action = running_follow(local_actor);
  if (!action || !natives.finish) {
    return pending;
  }
  stopping = true;
  stop_reason = reason;
  reinterpret_cast<Finish>(natives.finish)(action, kFinished);
  return true;
}

bool answer(const sdk::Value& request, void* entity_system, void* local_actor, void* local_entity) {
  const std::string op = kcd2s::integration::script_operation(request);
  if (op == "followCheck") {
    return can_start(entity_system, local_actor, local_entity, entity_of(request));
  }
  if (op == "followStart") {
    const std::uint32_t entity = entity_of(request);
    if (!can_start(entity_system, local_actor, local_entity, entity)) {
      return false;
    }
    if (running_follow(local_actor)) {
      stop(local_actor, "script");
    }
    reinterpret_cast<Start>(natives.start)(at<void*>(local_actor, kActorActions), &entity);
    const sdk::Value* hint = kcd2s::integration::script_field(request, "hint");
    const sdk::Value* can_stop = kcd2s::integration::script_field(request, "playerCanStop");
    const sdk::Value* run = kcd2s::integration::script_field(request, "run");
    show_hint = !hint || hint->tag() != sdk::Tag::Bool || hint->as_bool();
    player_can_stop = !can_stop || can_stop->tag() != sdk::Tag::Bool || can_stop->as_bool();
    run_pace.store(run && run->tag() == sdk::Tag::Bool && run->as_bool());
    const sdk::Value* distance = kcd2s::integration::script_field(request, "distance");
    float metres = 0.0f;
    if (distance && distance->tag() == sdk::Tag::Double) {
      metres = float(distance->as_double());
    } else if (distance && distance->tag() == sdk::Tag::Int) {
      metres = float(distance->as_int());
    }
    wanted_distance.store(metres >= kDistanceMin && metres <= kDistanceMax ? metres : 0.0f);
    target = entity;
    pending = true;
    active = false;
    stopping = false;
    own_stop = nullptr;
    stop_key_used = false;
    started_mounted = mounted_now;
    has_last_position = false;
    asked_at = seconds_now();
    return true;
  }
  if (op == "followStop") {
    if (!pending && !active) {
      return false;
    }
    if (pending && !running_follow(local_actor)) {
      pending = false;
      raise("stopped", "script");
      return true;
    }
    return stop(local_actor, "script");
  }
  return false;
}

bool movement_action(const char* action) {
  for (const char* name : {"moveforward", "moveback", "moveleft", "moveright"}) {
    if (std::strcmp(action, name) == 0) {
      return true;
    }
  }
  return false;
}

// The follow manager's check that the follow action asks every update: the vanilla
// follow runs only behind an NPC leading the player in a conversation, so a script's follow of a
// player passes here while it is the one this client started.
bool __fastcall hooked_may_follow(void* manager, std::uint32_t entity) {
  if ((pending || active) && !stopping && entity == target) {
    return true;
  }
  return original_may_follow(manager, entity);
}

// The follow manager's check that a follow which reached its target may go on when the
// target walks off again: it asks the check above for the conversation's partner, and a script's
// follow has none.
bool __fastcall hooked_may_keep_following(void* manager) {
  if (active && !stopping) {
    return true;
  }
  return original_may_keep_following(manager);
}

// The follow's movement request: the game sets bit 26, the walking pace, on it. With
// pace 'run' this client's follow lets the player run.
constexpr std::uint64_t kWalkPace = std::uint64_t(1) << 26;
using FollowMove = char(__fastcall*)(void* action, std::uint64_t* request, float* input,
                                     void* state);
FollowMove original_move = nullptr;

char __fastcall hooked_move(void* action, std::uint64_t* request, float* input, void* state) {
  const char result = original_move(action, request, input, state);
  if (!request || !active || stopping) {
    return result;
  }
  const float wanted = wanted_distance.load();
  if (wanted > 0.0f) {
    const float gap = gap_now.load();
    if (gap > wanted + kCatchUpFrom) {
      catching_up = true;
    } else if (gap < wanted + kCatchUpTo) {
      catching_up = false;
    }
  } else {
    catching_up = false;
  }
  const bool cuffed = kcds::runtime::input::cuffs::cuffed();
  if (run_pace.load() || (catching_up && !cuffed)) {
    *request &= ~kWalkPace;
  }
  return result;
}

// The follow's tuning for its target: for this client's follow with a distance, a copy
// with the stop distance (+ 16) and the set-off distance (+ 24) from it.
constexpr std::size_t kTuningSize = 64;
constexpr std::size_t kStopDistance = 16;
constexpr std::size_t kGoDistance = 24;
constexpr float kSetOffMargin = 0.5f;
using Tuning = const std::uint8_t*(__fastcall*)(void* manager, std::uint32_t entity);
Tuning original_tuning = nullptr;
thread_local std::uint8_t tuned[kTuningSize];

const std::uint8_t* __fastcall hooked_tuning(void* manager, std::uint32_t entity) {
  const std::uint8_t* game = original_tuning(manager, entity);
  const float wanted = wanted_distance.load();
  if (!game || wanted <= 0.0f || !(pending || active) || stopping || entity != target) {
    return game;
  }
  std::memcpy(tuned, game, kTuningSize);
  const float stop_at = at<float>(game, kStopDistance);
  const float go_at = at<float>(game, kGoDistance);
  // The game sets off again only at 5 m (stops at 1.5 m): a led prisoner trailed 5-8 m behind.
  // Here it sets off a little past the distance it keeps.
  const float go = wanted + kSetOffMargin;
  std::memcpy(tuned + kStopDistance, &wanted, sizeof(wanted));
  std::memcpy(tuned + kGoDistance, &go, sizeof(go));
  static bool logged = false;
  if (!logged && natives.log) {
    logged = true;
    natives.log("FOLLOW distance %.2f m (the game's: stops at %.2f, sets off at %.2f)", wanted,
                stop_at, go_at);
  }
  return tuned;
}

template <class Fn>
bool hook(void* target_function, void* detour, Fn* original) {
  if (!target_function) {
    return false;
  }
  return MH_CreateHook(target_function, detour, reinterpret_cast<void**>(original)) == MH_OK &&
         MH_EnableHook(target_function) == MH_OK;
}

}  // namespace

void install(const Natives& given) {
  natives = given;
  const bool hooked =
      hook(natives.may_follow, reinterpret_cast<void*>(&hooked_may_follow), &original_may_follow) &&
      hook(natives.may_keep_following, reinterpret_cast<void*>(&hooked_may_keep_following),
           &original_may_keep_following);
  if (!hooked) {
    // Without the checks' hooks the game would end every follow of a player at once.
    natives.start = nullptr;
  }
  // Without it a follow keeps the game's distance: Follow.start's distance does nothing.
  if (!hook(natives.tuning, reinterpret_cast<void*>(&hooked_tuning), &original_tuning) &&
      natives.log) {
    natives.log("FOLLOW distance unavailable: the follow's tuning was not found");
  }
  // Without it a follow only walks: pace 'run' does nothing.
  if (!hook(natives.move, reinterpret_cast<void*>(&hooked_move), &original_move) && natives.log) {
    natives.log("FOLLOW pace run unavailable: the follow's movement was not found");
  }
  if (natives.log) {
    natives.log("FOLLOW %s", natives.start && natives.finish ? "installed" : "unavailable");
  }
}

bool handle_runtime_message(const sdk::Value& message) {
  if (kcd2s::integration::script_operation(message) != "syncRequest") {
    return false;
  }
  const sdk::Value* id = kcd2s::integration::script_field(message, "id");
  const sdk::Value* request = kcd2s::integration::script_field(message, "request");
  if (!id || id->tag() != sdk::Tag::Int || !request ||
      kcd2s::integration::script_operation(*request).rfind("follow", 0) != 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (requests.size() < 64) {
    requests.push_back(Request{id->as_int(), *request});
  }
  return true;
}

bool on_action(const char* action, unsigned mode) {
  if (!action || !(pending || active) || stopping) {
    return false;
  }
  const bool movement = movement_action(action);
  const bool stop_key = std::strncmp(action, "chat_focus_follow_init", 22) == 0;
  if (!player_can_stop) {
    // The follow goes on: its keys do nothing (their releases still go through).
    return (movement || stop_key) && mode != 2;
  }
  if (mode == 1 && movement) {
    own_stop = "input";
  }
  if (stop_key) {
    stop_key_used = true;
  }
  return false;
}

// Notes a mount change or a teleport of the local player since the follow started.
void watch_local_player(void* local_entity) {
  if (own_stop) {
    return;
  }
  if (mounted_now != started_mounted) {
    own_stop = "mountChanged";
    return;
  }
  if (!local_entity) {
    return;
  }
  float position[3];
  props::entity::world_position(local_entity, position);
  if (has_last_position) {
    const float dx = position[0] - last_position[0];
    const float dy = position[1] - last_position[1];
    const float dz = position[2] - last_position[2];
    if (dx * dx + dy * dy + dz * dz > kTeleportDistance * kTeleportDistance) {
      own_stop = "teleported";
    }
  }
  std::memcpy(last_position, position, sizeof(last_position));
  has_last_position = true;
}

void frame(void* entity_system, void* local_actor, void* local_entity, bool mounted) {
  mounted_now = mounted;
  kcds::runtime::action_hints::hide_game_hint(kStopHintAction, (pending || active) && !show_hint);
  std::vector<Request> taken;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    taken.swap(requests);
  }
  std::vector<Reply> answered;
  for (const Request& request : taken) {
    answered.push_back(
        Reply{request.id, answer(request.body, entity_system, local_actor, local_entity)});
  }
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    for (const Reply& reply : answered) {
      replies.push_back(reply);
    }
  }
  if (!pending && !active) {
    own_stop = nullptr;
    return;
  }
  if (!stopping) {
    watch_local_player(local_entity);
  }
  // The gap to the target, for the catch-up of a follow with a distance.
  void* leader = natives.get_entity ? natives.get_entity(entity_system, target) : nullptr;
  if (leader && local_entity) {
    float from[3];
    float to[3];
    props::entity::world_position(local_entity, from);
    props::entity::world_position(leader, to);
    gap_now.store(std::hypot(to[0] - from[0], to[1] - from[1]));
  }
  if (own_stop) {
    const char* reason = own_stop;
    own_stop = nullptr;
    if (pending && !running_follow(local_actor)) {
      pending = false;
      raise("stopped", reason);
      return;
    }
    stop(local_actor, reason);
  }
  const bool running = local_actor && running_follow(local_actor) != nullptr;
  if (pending && running) {
    pending = false;
    active = true;
    raise("started", "");
    return;
  }
  if (pending && seconds_now() - asked_at > kStartGraceSeconds) {
    pending = false;
    raise("stopped", "failed");
    return;
  }
  if (active && !running) {
    active = false;
    const bool lost = !natives.get_entity || !natives.get_entity(entity_system, target);
    const char* reason = lost ? "targetLost" : "interrupted";
    if (stopping) {
      reason = stop_reason;
    } else if (stop_key_used) {
      reason = "input";
    }
    raise("stopped", reason);
    stopping = false;
  }
}

void pump(const RuntimeSender& send) {
  std::vector<Reply> taken_replies;
  std::vector<Event> taken_events;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    taken_replies.swap(replies);
    taken_events.swap(events);
  }
  for (const Reply& reply : taken_replies) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("syncReply"))},
                                    {"id", sdk::Value(reply.id)},
                                    {"value", sdk::Value(reply.value)}}));
  }
  for (const Event& event : taken_events) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("followEvent"))},
                                    {"event", sdk::Value(event.event)},
                                    {"reason", sdk::Value(event.reason)}}));
  }
}

}  // namespace kcds::runtime::follow
