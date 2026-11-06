#include "local_npcs.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../animation/player_animation.hpp"
#include "../body_state/body_readings.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "npc_body.hpp"
#include "npc_speech.hpp"
#include "npc_walk.hpp"

namespace kcds::runtime::npcs::local {
namespace {

namespace sdk = kcd2s::sdk;

// IEntitySystem GetEntity (slot 14) and RemoveEntity (slot 19); IEntity SetWorldTM (slot 30);
// IActorSystem GetActor (slot 3).
constexpr std::size_t kGetEntitySlot = 14;
constexpr std::size_t kRemoveEntitySlot = 19;
constexpr std::size_t kSetWorldTmSlot = 30;
constexpr std::size_t kGetActorSlot = 3;
// The entity id's place in an IEntity.
constexpr std::size_t kEntityIdOffset = 12;
// Requests waiting for the engine thread; a flood from a script is cut off.
constexpr std::size_t kMaxQueued = 1024;
// A local NPC's fragment is played under its id with this bit (player_animation's bodies), apart
// from the server NPCs' network ids.
constexpr std::uint32_t kLocalFragmentKeys = 0x80000000u;
// A local NPC's walk is reported under its id with this bit (npc_walk's npc numbers), apart from
// the server NPCs' network ids.
constexpr std::uint32_t kLocalWalkKeys = 0x40000000u;
// IEntity GetWorldTM (slot 32): its translation is the body's place.
constexpr std::size_t kWorldTmSlot = 32;

// One request of the resource runtime, as it came.
struct Request {
  std::string operation;
  std::int64_t id{};
  std::string soul;
  std::string actor_class;
  std::string name;
  std::string text;
  float position[3]{};
  float rotation[4]{0, 0, 0, 1};
  int speed = 1;
};

struct Body {
  std::uint32_t engine_id{};
  void* actor{};
  std::string name;
  std::string appearance;
  Speech speech;
  // A fragment plays: watched once a frame, so a loop goes on.
  bool fragment = false;
  std::uint32_t walk_sequence = 0;
};

std::uint32_t fragment_key(std::int64_t id) {
  return kLocalFragmentKeys | std::uint32_t(id);
}

Logger log_line = nullptr;
std::mutex queue_mutex;
std::vector<Request> queued;
std::map<std::int64_t, Body> bodies;
// The walk states by the local NPC's id, and the questions about them (any thread).
std::mutex walk_mutex;
std::map<std::int64_t, std::string> walk_states;
std::vector<std::pair<std::int64_t, std::int64_t>> walk_questions;

void set_walk_state(std::int64_t id, const char* state) {
  std::lock_guard<std::mutex> lock(walk_mutex);
  walk_states[id] = state;
}

template <class Function>
Function slot(void* object, std::size_t index) {
  return reinterpret_cast<Function>((*static_cast<void***>(object))[index]);
}

void* entity_of(void* entity_system, std::uint32_t engine_id) {
  if (!entity_system || !engine_id) {
    return nullptr;
  }
  return slot<void*(__fastcall*)(void*, std::uint32_t)>(entity_system, kGetEntitySlot)(
      entity_system, engine_id);
}

double number(const sdk::Value* value) {
  if (!value) {
    return 0;
  }
  if (value->tag() == sdk::Tag::Double) {
    return value->as_double();
  }
  if (value->tag() == sdk::Tag::Int) {
    return double(value->as_int());
  }
  return 0;
}

std::string text(const sdk::Value& message, const char* name) {
  const sdk::Value* value = kcd2s::integration::script_field(message, name);
  return value && value->tag() == sdk::Tag::Str ? value->as_str() : std::string();
}

// Reads { x, y, z } (and w) of a field into `out`; false without them all.
bool read_vector(const sdk::Value& message, const char* name, float* out, bool with_w) {
  const sdk::Value* vector = kcd2s::integration::script_field(message, name);
  if (!vector || vector->tag() != sdk::Tag::Map) {
    return false;
  }
  const char* keys[] = {"x", "y", "z", "w"};
  const std::size_t count = with_w ? 4 : 3;
  for (std::size_t i = 0; i < count; ++i) {
    const sdk::Value* part = kcd2s::integration::script_field(*vector, keys[i]);
    const double value = number(part);
    if (!part || !std::isfinite(value)) {
      return false;
    }
    out[i] = float(value);
  }
  return true;
}

void set_transform(void* entity, const float* p, const float* q) {
  const float x = q[0];
  const float y = q[1];
  const float z = q[2];
  const float w = q[3];
  const float m[12] = {
      1 - 2 * (y * y + z * z), 2 * (x * y - w * z),     2 * (x * z + w * y),     p[0],
      2 * (x * y + w * z),     1 - 2 * (x * x + z * z), 2 * (y * z - w * x),     p[1],
      2 * (x * z - w * y),     2 * (y * z + w * x),     1 - 2 * (x * x + y * y), p[2],
  };
  slot<void(__fastcall*)(void*, const float*, unsigned)>(entity, kSetWorldTmSlot)(entity, m, 0);
}

void remove_body(void* entity_system, std::int64_t id, Body& body) {
  end_speech(body.speech);
  if (entity_of(entity_system, body.engine_id)) {
    player_animation::stop_on_body(fragment_key(id));
    slot<void(__fastcall*)(void*, std::uint32_t, bool)>(entity_system, kRemoveEntitySlot)(
        entity_system, body.engine_id, false);
  }
  if (log_line) {
    log_line("LOCAL NPC removed id=%lld", static_cast<long long>(id));
  }
}

void create(void* actor_system, const Request& request) {
  char name[64];
  std::snprintf(name, sizeof(name), "kcds_local_npc_%lld", static_cast<long long>(request.id));
  void* entity = spawn_body(name, request.actor_class.c_str(), request.soul, request.position,
                            request.rotation);
  if (!entity) {
    if (log_line) {
      log_line("LOCAL NPC refused id=%lld class=%s soul=%s", static_cast<long long>(request.id),
               request.actor_class.c_str(), request.soul.c_str());
    }
    return;
  }
  Body body;
  body.engine_id =
      *reinterpret_cast<const std::uint32_t*>(static_cast<std::uint8_t*>(entity) + kEntityIdOffset);
  body.actor = actor_system ? slot<void*(__fastcall*)(void*, std::uint32_t)>(
                                  actor_system, kGetActorSlot)(actor_system, body.engine_id)
                            : nullptr;
  body.name = request.name;
  bodies[request.id] = body;
  if (log_line) {
    log_line("LOCAL NPC id=%lld engine=0x%x class=%s soul=%s", static_cast<long long>(request.id),
             body.engine_id, request.actor_class.c_str(), request.soul.c_str());
  }
}

void apply(void* actor_system, void* entity_system, const Request& request, double now) {
  if (request.operation == "localNpcCreate") {
    auto old = bodies.find(request.id);
    if (old != bodies.end()) {
      remove_body(entity_system, old->first, old->second);
      bodies.erase(old);
    }
    create(actor_system, request);
    return;
  }
  auto found = bodies.find(request.id);
  if (found == bodies.end()) {
    return;
  }
  Body& body = found->second;
  void* entity = entity_of(entity_system, body.engine_id);
  if (request.operation == "localNpcRemove") {
    remove_body(entity_system, found->first, body);
    bodies.erase(found);
  } else if (request.operation == "localNpcPlace" && entity) {
    set_transform(entity, request.position, request.rotation);
  } else if (request.operation == "localNpcAppearance") {
    body.appearance = request.text;
    const bool built = apply_appearance_field(body.actor, request.text);
    if (log_line) {
      log_line("LOCAL NPC appearance id=%lld %s %s", static_cast<long long>(request.id),
               request.text.c_str(), built ? "built" : "refused");
    }
  } else if (request.operation == "localNpcAnimation" && entity) {
    player_animation::stop_on_body(fragment_key(request.id));
    body.fragment = false;
    FragmentField fragment;
    if (fragment_field_of(request.text, fragment)) {
      apply_animation_field(entity, std::string());
      body.fragment = player_animation::play_on_body(
          fragment_key(request.id), body_state::action_controller(body.actor), fragment.fragment,
          fragment.tags, fragment.loop, fragment.sequence);
      if (log_line) {
        log_line("LOCAL NPC animation id=%lld fragment %s tags '%s' %s",
                 static_cast<long long>(request.id), fragment.fragment.c_str(),
                 fragment.tags.c_str(), body.fragment ? "queued" : "refused (no such fragment)");
      }
      return;
    }
    const bool played = apply_animation_field(entity, request.text);
    if (log_line) {
      log_line("LOCAL NPC animation id=%lld %s %s", static_cast<long long>(request.id),
               request.text.empty() ? "stop" : request.text.c_str(),
               played ? "plays" : "not in the body's animations");
    }
  } else if (request.operation == "localNpcSay") {
    take_speech(body.speech, request.text, body.name, now);
  } else if (request.operation == "localNpcWalk" && entity) {
    const float* place = slot<const float*(__fastcall*)(void*)>(entity, kWorldTmSlot)(entity);
    const float from[3] = {place ? place[3] : 0.0f, place ? place[7] : 0.0f,
                           place ? place[11] : 0.0f};
    body.walk_sequence += 1;
    const bool started =
        place && walk::walk_to(entity, kLocalWalkKeys | std::uint32_t(request.id),
                               body.walk_sequence, from, request.position, request.speed);
    set_walk_state(request.id, started ? "walking" : "refused");
    if (log_line) {
      log_line("LOCAL NPC walk id=%lld to %.1f %.1f %.1f %s", static_cast<long long>(request.id),
               request.position[0], request.position[1], request.position[2],
               started ? "started" : "refused (no agent)");
    }
  }
}

}  // namespace

void install(Logger logger) {
  log_line = logger;
}

bool handle_runtime_message(const sdk::Value& message) {
  const std::string operation = kcd2s::integration::script_operation(message);
  if (operation == "syncRequest") {
    const sdk::Value* id = kcd2s::integration::script_field(message, "id");
    const sdk::Value* request = kcd2s::integration::script_field(message, "request");
    if (!id || id->tag() != sdk::Tag::Int || !request ||
        kcd2s::integration::script_operation(*request) != "localNpcWalkState") {
      return false;
    }
    const std::int64_t npc = std::int64_t(number(kcd2s::integration::script_field(*request, "id")));
    std::lock_guard<std::mutex> lock(walk_mutex);
    if (walk_questions.size() < kMaxQueued) {
      walk_questions.emplace_back(id->as_int(), npc);
    }
    return true;
  }
  if (operation.rfind("localNpc", 0) != 0) {
    return false;
  }
  Request request;
  request.operation = operation;
  request.id = std::int64_t(number(kcd2s::integration::script_field(message, "id")));
  if (operation == "localNpcCreate") {
    request.soul = text(message, "soul");
    request.actor_class = text(message, "actorClass");
    request.name = text(message, "name");
    if (!read_vector(message, "position", request.position, false)) {
      return true;
    }
    read_vector(message, "rotation", request.rotation, true);
  } else if (operation == "localNpcPlace") {
    if (!read_vector(message, "position", request.position, false) ||
        !read_vector(message, "rotation", request.rotation, true)) {
      return true;
    }
  } else if (operation == "localNpcAppearance") {
    request.text = text(message, "appearance");
  } else if (operation == "localNpcAnimation") {
    request.text = text(message, "animation");
  } else if (operation == "localNpcSay") {
    request.text = text(message, "speech");
  } else if (operation == "localNpcWalk") {
    if (!read_vector(message, "position", request.position, false)) {
      return true;
    }
    const double speed = number(kcd2s::integration::script_field(message, "speed"));
    request.speed = speed >= 1 && speed <= 3 ? int(speed) : 1;
  } else if (operation != "localNpcRemove") {
    return false;
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (queued.size() < kMaxQueued) {
    queued.push_back(std::move(request));
  }
  return true;
}

void frame(void* actor_system, void* entity_system, const float* view, double now) {
  std::vector<Request> requests;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    requests.swap(queued);
  }
  for (const Request& request : requests) {
    apply(actor_system, entity_system, request, now);
  }
  for (auto it = bodies.begin(); it != bodies.end();) {
    void* entity = entity_of(entity_system, it->second.engine_id);
    if (!entity) {
      // The game removed the body itself.
      end_speech(it->second.speech);
      player_animation::forget_body(fragment_key(it->first));
      it = bodies.erase(it);
      continue;
    }
    if (it->second.fragment) {
      // A loop is queued again; a one-shot that ended stays ended (local NPCs raise no events).
      void* controller = body_state::action_controller(it->second.actor);
      if (player_animation::watch_body(fragment_key(it->first), controller)) {
        it->second.fragment = false;
      }
    }
    show_speech(it->second.speech, entity, view, now);
    ++it;
  }
}

namespace {

// The editor's word for how a walk ended (npc_walk.hpp's statuses).
const char* walk_state_of(std::uint8_t status) {
  if (status == walk::kWalkReached) {
    return "reached";
  }
  if (status == walk::kWalkNoPath || status == walk::kWalkOffMesh) {
    return "no path";
  }
  if (status == walk::kWalkReplaced) {
    return "walking";
  }
  return "stopped";
}

}  // namespace

void take_walks() {
  for (const walk::Finished& done : walk::take_finished()) {
    if ((done.npc & kLocalWalkKeys) == 0) {
      continue;
    }
    const std::int64_t id = std::int64_t(done.npc & ~kLocalWalkKeys);
    const char* state = walk_state_of(done.status);
    set_walk_state(id, state);
    if (log_line) {
      log_line("LOCAL NPC walk id=%lld ended: %s (game status %u)", static_cast<long long>(id),
               state, unsigned(done.status));
    }
  }
}

void pump(const std::function<bool(const sdk::Value&)>& send) {
  std::vector<std::pair<std::int64_t, sdk::Value>> answers;
  {
    std::lock_guard<std::mutex> lock(walk_mutex);
    for (const auto& [question, npc] : walk_questions) {
      const auto found = walk_states.find(npc);
      answers.emplace_back(question,
                           sdk::Value(found == walk_states.end() ? std::string() : found->second));
    }
    walk_questions.clear();
  }
  for (auto& [question, value] : answers) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("syncReply"))},
                                    {"id", sdk::Value(question)},
                                    {"value", std::move(value)}}));
  }
}

void clear(void* entity_system) {
  for (auto& [id, body] : bodies) {
    remove_body(entity_system, id, body);
  }
  bodies.clear();
  std::lock_guard<std::mutex> lock(queue_mutex);
  queued.clear();
}

}  // namespace kcds::runtime::npcs::local
