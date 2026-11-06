#include "npc_walk.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace kcds::runtime::npcs::walk {
namespace {

// The game's movement speeds (E_MovementSpeed, named in): 0 RelaxedWalk, 1 Walk,
// 2 AlertedWalk, 3 Run, 4 FastRun, 5 to 7 the sprints, 8 to 11 the dashes. An order's walk, jog
// and run are 1, 2 and 3; a dog's Run is a trot and its AlertedWalk a walk (2.2 m/s on run),
// so a dog's jog is Run and its run Sprint, as the game's dog packs hurry.
constexpr int kSpeedRun = 3;
constexpr int kSpeedSprint = 7;
constexpr int kOrderJog = 2;
constexpr int kOrderRun = 3;
// IEntity GetClass (slot 3), IEntityClass GetName (slot 2).
constexpr std::size_t kEntityClassSlot = 3;
constexpr std::size_t kClassNameSlot = 2;

// The AI module (at the global's +120) and its movement task manager (slot 10).
constexpr std::size_t kAiModule = 120;
constexpr std::size_t kTaskManagerSlot = 10;
// The agent's type for path finding (slot 35, returned through a buffer).
constexpr std::size_t kAgentTypeSlot = 35;

// S_MovementTaskRequest (about 600 bytes; the game builds it on its stack): the agent at 0, the
// path request it owns at 8 (16 holds an already planned path instead), the movement speed at
// 248.
constexpr std::size_t kRequestSize = 768;
constexpr std::size_t kRequestPath = 8;
constexpr std::size_t kRequestSpeed = 248;
// S_PathFindingRequestDefault (152 bytes): its clone onto the game's heap (slot 2), the agent
// type at 8, the start at 88, the destination at 120.
constexpr std::size_t kPathRequestSize = 192;
constexpr std::size_t kPathCloneSlot = 2;
constexpr std::size_t kPathAgentType = 8;
constexpr std::size_t kPathStart = 88;
constexpr std::size_t kPathDestination = 120;
// The request's callbacks, std::functions the task calls and so must hold something: when the
// task ends (with its S_MovementTaskResult) at 344, and the other (with a task id) at 536.
constexpr std::size_t kRequestFinished = 344;
constexpr std::size_t kRequestProgress = 536;

// What the game's callback builders bind: a member function, the
// placeholder for its argument, and the object it is called on (here the NPC's id).
struct Binder {
  void* function;
  std::uint64_t placeholder;
  void* object;
};
// S_MovementTaskResult: the task's id and how it ended.
struct TaskResult {
  std::uint32_t task;
  std::uint8_t status;
};

using AgentOfEntity = void*(__fastcall*)(void* unused, void* entity);
using RequestInit = void*(__fastcall*)(void* request, void* agent);
using RequestCleanup = void(__fastcall*)(void* request);
using PathRequestInit = void*(__fastcall*)(void* request);
using StartTask = int(__fastcall*)(void* manager, void* request);
using StopTasks = void(__fastcall*)(void* manager, void* agent, bool finish, bool force);
using Get = void*(__fastcall*)(void*);
using AgentType = const std::int32_t*(__fastcall*)(void* agent, void* out);
using Clone = void*(__fastcall*)(void* request);
using BindCallback = void*(__fastcall*)(void* function, const Binder* binder);

Natives natives;
bool installed = false;
std::mutex finished_mutex;
std::vector<Finished> finished;

// The task ended: kept for take_finished (the game may call this from its AI update).
void __fastcall walk_finished(void* object, const TaskResult* result) {
  const auto packed = reinterpret_cast<std::uintptr_t>(object);
  std::lock_guard<std::mutex> lock(finished_mutex);
  finished.push_back({static_cast<std::uint32_t>(packed), static_cast<std::uint32_t>(packed >> 32),
                      result ? result->status : std::uint8_t(0xff)});
}

void __fastcall walk_progress(void*, std::uint32_t) {
}

template <class Function>
Function slot(void* object, std::size_t index) {
  return reinterpret_cast<Function>((*static_cast<void***>(object))[index]);
}

void* task_manager() {
  void* owner = natives.ai_module_global ? *natives.ai_module_global : nullptr;
  void* module =
      owner ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(owner) + kAiModule) : nullptr;
  return module ? slot<Get>(module, kTaskManagerSlot)(module) : nullptr;
}

// The text up to the next '|' from `at`, `at` moved past it.
std::string next_part(const std::string& text, std::size_t& at) {
  const std::size_t end = text.find('|', at);
  std::string part = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
  at = end == std::string::npos ? text.size() : end + 1;
  return part;
}

}  // namespace

void* agent_of(void* entity) {
  if (!installed || !entity) {
    return nullptr;
  }
  return reinterpret_cast<AgentOfEntity>(natives.agent_of_entity)(nullptr, entity);
}

bool install(const Natives& given) {
  natives = given;
  installed = natives.ai_module_global && natives.agent_of_entity && natives.request_init &&
              natives.request_cleanup && natives.path_request_init && natives.start_task &&
              natives.stop_tasks && natives.bind_finished && natives.bind_progress;
  return installed;
}

bool parse_order(const std::string& intent, Order& order) {
  std::size_t at = 0;
  order.sequence =
      static_cast<std::uint32_t>(std::strtoul(next_part(intent, at).c_str(), nullptr, 10));
  order.kind = next_part(intent, at);
  if (order.kind == "attack") {
    // "<sequence>|attack|<targetId>|<weapon>"
    order.target =
        static_cast<std::uint32_t>(std::strtoul(next_part(intent, at).c_str(), nullptr, 10));
    order.bow = next_part(intent, at) == "bow";
    return order.target != 0;
  }
  if (order.kind == "follow") {
    // "<sequence>|follow|<targetId>|<speed>|<radius>"
    const std::string target = next_part(intent, at);
    if (target.empty()) {
      return false;
    }
    order.target = static_cast<std::uint32_t>(std::strtoul(target.c_str(), nullptr, 10));
  } else if (order.kind == "lookAt") {
    // "<sequence>|lookAt|entity|<id>" or "<sequence>|lookAt|point|<x>|<y>|<z>"
    const std::string what = next_part(intent, at);
    if (what == "entity") {
      order.target =
          static_cast<std::uint32_t>(std::strtoul(next_part(intent, at).c_str(), nullptr, 10));
      return order.target != 0;
    }
    for (float& coordinate : order.destination) {
      const std::string part = next_part(intent, at);
      if (part.empty()) {
        return false;
      }
      coordinate = std::strtof(part.c_str(), nullptr);
    }
    return what == "point";
  } else if (order.kind == "moveTo" || order.kind == "flee") {
    // "<sequence>|moveTo|<x>|<y>|<z>|<speed>|<radius>|<pathfinding>", and flee's point is the
    // one it runs from: "<sequence>|flee|<x>|<y>|<z>|<speed>|<radius>".
    for (float& coordinate : order.destination) {
      const std::string part = next_part(intent, at);
      if (part.empty()) {
        return false;
      }
      coordinate = std::strtof(part.c_str(), nullptr);
    }
  } else {
    return !order.kind.empty();
  }
  const std::string speed = next_part(intent, at);
  order.speed = speed == "run" ? 3 : speed == "jog" ? 2 : 1;
  const std::string radius = next_part(intent, at);
  if (!radius.empty()) {
    order.radius = std::strtof(radius.c_str(), nullptr);
  }
  return true;
}

std::vector<Finished> take_finished() {
  std::lock_guard<std::mutex> lock(finished_mutex);
  return std::exchange(finished, {});
}

// Whether the entity is one of the game's dogs.
bool dog(void* entity) {
  using GetClass = void*(__fastcall*)(void*);
  using GetName = const char*(__fastcall*)(void*);
  void* entity_class = slot<GetClass>(entity, kEntityClassSlot)(entity);
  const char* name = entity_class ? slot<GetName>(entity_class, kClassNameSlot)(entity_class) : "";
  return name && (std::strcmp(name, "Dog") == 0 || std::strcmp(name, "WildDog") == 0);
}

// The game's movement speed of an order's pace for this body.
int movement_speed(void* entity, int speed) {
  if (!dog(entity)) {
    return speed;
  }
  if (speed == kOrderRun) {
    return kSpeedSprint;
  }
  return speed == kOrderJog ? kSpeedRun : speed;
}

bool walk_to(void* entity, std::uint32_t npc, std::uint32_t sequence, const float from[3],
             const float destination[3], int speed) {
  if (!installed) {
    return false;
  }
  speed = movement_speed(entity, speed);
  void* manager = task_manager();
  void* agent = agent_of(entity);
  if (!manager || !agent) {
    return false;
  }
  // The path request, as the game's move node fills it: the defaults, the agent's
  // type, where from and where to. This one lives here only to be cloned; its members hold
  // nothing to free.
  alignas(16) std::uint8_t path[kPathRequestSize] = {};
  reinterpret_cast<PathRequestInit>(natives.path_request_init)(path);
  alignas(16) std::uint8_t type[16] = {};
  const std::int32_t agent_type = *slot<AgentType>(agent, kAgentTypeSlot)(agent, type);
  std::memcpy(path + kPathAgentType, &agent_type, sizeof(agent_type));
  std::memcpy(path + kPathStart, from, 3 * sizeof(float));
  std::memcpy(path + kPathDestination, destination, 3 * sizeof(float));
  // The request owns a clone of it on the game's heap (as the move node's makes one),
  // which the request's cleanup deletes.
  void* owned = slot<Clone>(path, kPathCloneSlot)(path);

  alignas(16) std::uint8_t request[kRequestSize] = {};
  reinterpret_cast<RequestInit>(natives.request_init)(request, agent);
  std::memcpy(request + kRequestPath, &owned, sizeof(owned));
  std::memcpy(request + kRequestSpeed, &speed, sizeof(speed));
  // Both callbacks, as the game's own walk request has them; an empty one ends the
  // game when the task calls it.
  // The object the callbacks get is the NPC and its order's sequence, packed.
  void* object = reinterpret_cast<void*>(std::uintptr_t(npc) | std::uintptr_t(sequence) << 32);
  const Binder on_finished{reinterpret_cast<void*>(&walk_finished), 0, object};
  const Binder on_progress{reinterpret_cast<void*>(&walk_progress), 0, object};
  reinterpret_cast<BindCallback>(natives.bind_finished)(request + kRequestFinished, &on_finished);
  reinterpret_cast<BindCallback>(natives.bind_progress)(request + kRequestProgress, &on_progress);
  const int task = reinterpret_cast<StartTask>(natives.start_task)(manager, request);
  reinterpret_cast<RequestCleanup>(natives.request_cleanup)(request);
  return task != -1;
}

void stop(void* entity) {
  if (!installed) {
    return;
  }
  void* manager = task_manager();
  void* agent = agent_of(entity);
  if (manager && agent) {
    reinterpret_cast<StopTasks>(natives.stop_tasks)(manager, agent, true, false);
  }
}

}  // namespace kcds::runtime::npcs::walk
