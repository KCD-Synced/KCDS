#include "dress_cover.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <unordered_set>

#include "../../physics/local_world.hpp"
#include "../../props/prop_entity.hpp"

namespace kcds::runtime::npcs::reactions::dress_cover {
namespace {

using Clock = std::chrono::steady_clock;

constexpr char kContext[] = "speech_disableNakedPlayerReactionsForNPC";
// The NPCs this near the player have the context; they are looked for this often.
constexpr float kReach = 40.0f;
constexpr auto kLookEvery = std::chrono::seconds(1);
// The flag the game's own rules pass with a count change.
constexpr char kCountFlag = 6;
// The manager holds its changes back while this byte is set (and flushes them itself then).
constexpr std::size_t kManagerHolding = 448;
// IEntity: its id (+ 12), GetPhysics (slot 79); IPhysicalEntity GetType (slot 1), 4 a living body.
constexpr std::size_t kEntityId = 12;
constexpr std::size_t kGetPhysics = 79;
constexpr std::size_t kPhysicsType = 1;
constexpr int kLivingBody = 4;

using Manager = void*(__fastcall*)();
using ContextId = std::uint32_t*(__fastcall*)(void* unused, std::uint32_t* id, void* name);
using CountChange = void*(__fastcall*)(void* manager, void* entity, std::uint32_t id, char flag);
using Flush = void(__fastcall*)(void* manager);
using Has = bool(__fastcall*)(void* manager, void* entity, void* name);
using StringFromText = void*(__fastcall*)(void* string, const char* text);
using StringFree = void(__fastcall*)(void* string);

Natives natives;
Logger log_line = nullptr;
bool ready = false;
bool id_known = false;
std::uint32_t context_id = 0;
// The bodies that have the context from this game, by entity id.
std::unordered_set<std::uint32_t> covered;
Clock::time_point next_look;

template <class Fn>
Fn slot(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*static_cast<void***>(object))[index]);
}

std::uint32_t id_of(void* entity) {
  std::uint32_t id = 0;
  std::memcpy(&id, static_cast<std::uint8_t*>(entity) + kEntityId, sizeof(id));
  return id;
}

bool living(void* entity) {
  void* physics = slot<void*(__fastcall*)(void*)>(entity, kGetPhysics)(entity);
  return physics && slot<int(__fastcall*)(void*)>(physics, kPhysicsType)(physics) == kLivingBody;
}

// The context's id, looked up once by its name (a game string made and freed for it).
bool find_context_id() {
  if (id_known) {
    return true;
  }
  void* name = nullptr;
  reinterpret_cast<StringFromText>(natives.string_from_text)(&name, kContext);
  reinterpret_cast<ContextId>(natives.context_id)(nullptr, &context_id, &name);
  reinterpret_cast<StringFree>(natives.string_free)(&name);
  id_known = true;
  if (log_line) {
    log_line("DRESS COVER context %s is id %u", kContext, context_id);
  }
  return true;
}

void change(void* manager, void* entity, bool add) {
  auto count = reinterpret_cast<CountChange>(add ? natives.add : natives.remove);
  count(manager, entity, context_id, kCountFlag);
}

void flush(void* manager) {
  if (*(static_cast<std::uint8_t*>(manager) + kManagerHolding) == 0) {
    reinterpret_cast<Flush>(natives.flush)(manager);
  }
}

// Whether the game now sees the context on that body (its own check, by the context's name).
bool seen_on(void* manager, void* entity) {
  void* name = nullptr;
  reinterpret_cast<StringFromText>(natives.string_from_text)(&name, kContext);
  const bool has = reinterpret_cast<Has>(natives.has)(manager, entity, &name);
  reinterpret_cast<StringFree>(natives.string_free)(&name);
  return has;
}

// Takes the context from every body that has it from this game.
void uncover_all(void* manager) {
  bool changed = false;
  for (const std::uint32_t id : covered) {
    if (void* entity = props::entity::by_id(id)) {
      change(manager, entity, false);
      changed = true;
    }
  }
  covered.clear();
  if (changed) {
    flush(manager);
  }
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  ready = natives.manager && natives.context_id && natives.add && natives.remove && natives.flush &&
          natives.has && natives.string_from_text && natives.string_free;
  return ready;
}

void tick(void* local_entity, bool woman) {
  const Clock::time_point now = Clock::now();
  if (!ready || !local_entity || now < next_look) {
    return;
  }
  next_look = now + kLookEvery;
  void* manager = reinterpret_cast<Manager>(natives.manager)();
  if (!manager) {
    return;
  }
  if (!woman) {
    uncover_all(manager);
    return;
  }
  find_context_id();
  float at[3] = {};
  props::entity::world_position(local_entity, at);
  const std::uint32_t own = id_of(local_entity);
  std::unordered_set<std::uint32_t> near;
  bool changed = false;
  std::size_t added = 0;
  void* first_added = nullptr;
  for (void* entity : physics::physical_entities_near(at, kReach)) {
    const std::uint32_t id = id_of(entity);
    if (id == own || !living(entity) || !near.insert(id).second) {
      continue;
    }
    if (covered.insert(id).second) {
      change(manager, entity, true);
      changed = true;
      first_added = first_added ? first_added : entity;
      ++added;
    }
  }
  std::size_t taken = 0;
  for (auto it = covered.begin(); it != covered.end();) {
    if (near.count(*it) != 0) {
      ++it;
      continue;
    }
    if (void* entity = props::entity::by_id(*it)) {
      change(manager, entity, false);
      changed = true;
      ++taken;
    }
    it = covered.erase(it);
  }
  if (changed) {
    flush(manager);
  }
  if (log_line && (added > 0 || taken > 0)) {
    log_line("DRESS COVER %zu bodies near the player now have the context (+%zu, -%zu)%s",
             covered.size(), added, taken,
             first_added ? (seen_on(manager, first_added) ? ", the game sees it"
                                                          : ", the game does NOT see it")
                         : "");
  }
}

void reset() {
  covered.clear();
  next_look = {};
}

}  // namespace kcds::runtime::npcs::reactions::dress_cover
