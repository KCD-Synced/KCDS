#include "pot_looks.hpp"

#include <windows.h>

#include <cstring>
#include <map>
#include <utility>

#include "MinHook.h"

namespace kcds::runtime::cookpot {
namespace {

// Where SetPrefabPhaseById keeps what it uses: the entity system and the prefab
// system globals (RIP-relative loads: displacement offset, next instruction), and its calls.
constexpr std::size_t kEntitySystemDisplacement = 0x1B;
constexpr std::size_t kEntitySystemNext = 0x1F;
constexpr std::size_t kPrefabSystemDisplacement = 0x34;
constexpr std::size_t kPrefabSystemNext = 0x38;
constexpr std::size_t kFindCall = 0x54;
constexpr std::size_t kLibraryOwnerCall = 0x63;
constexpr std::size_t kNameCall = 0x7E;
constexpr std::size_t kLookupCall = 0x91;
constexpr std::size_t kQueueCall = 0xB9;
// The prefab system's runtime prefab manager, its set of runtime prefabs, and the owner's prefab
// library.
constexpr std::size_t kRuntimePrefabs = 336;
constexpr std::size_t kRuntimePrefabSet = 8;
constexpr std::size_t kLibraryOwnerSystems = 224;
constexpr std::size_t kSystemsLibrary = 296;
// The entity system's member the swaps are queued to.
constexpr std::size_t kEntitySystemQueue = 8;

const char* const kHomeCategory = "fireplace_cauldron_home";
const char* const kCampCategory = "fireplace_cauldron_camp";

// A queued swap: the runtime prefab manager, the cauldron's entity id and the phase's template.
struct Swap {
  void* manager = nullptr;
  std::uint32_t entity = 0;
  std::uint32_t pad = 0;
  void* prefab = nullptr;
};
static_assert(sizeof(Swap) == 24);

// The game's std::string (MSVC layout): inline text or a pointer to it, the size, the capacity.
struct GameString {
  std::uint8_t storage[16] = {};
  std::uint64_t size = 0;
  std::uint64_t capacity = 0;
};
static_assert(sizeof(GameString) == 32);

using Find = void*(__fastcall*)(void* set, const std::uint32_t* entity);
using LibraryOwner = void*(__fastcall*)();
using MakeName = void*(__fastcall*)(void* name, const char* text);
using Lookup = void*(__fastcall*)(void* library, void* name, int phase);
using Queue = char(__fastcall*)(void* queue, const Swap* swap);

Natives natives;
void** entity_system_slot = nullptr;
void** prefab_system_slot = nullptr;
Find find_prefab = nullptr;
LibraryOwner library_owner = nullptr;
MakeName make_name = nullptr;
Lookup lookup = nullptr;
Queue original_queue = nullptr;
// entity id -> the template the server's look shows
std::map<std::uint32_t, void*> held;
// (camp, phase) -> template
std::map<std::pair<bool, std::uint8_t>, void*> templates;

template <class... Args>
void log_line(const char* format, Args... args) {
  if (natives.log) {
    natives.log(format, args...);
  }
}

template <class T>
T read(const void* object, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

// The target of a rel32 call at `offset` in the binding, or nullptr when it is not a call.
void* call_target(const std::uint8_t* code, std::size_t offset) {
  if (code[offset] != 0xE8) {
    return nullptr;
  }
  return const_cast<std::uint8_t*>(code + offset + 5 + read<std::int32_t>(code, offset + 1));
}

// The global a RIP-relative load at `displacement` (next instruction at `next`) reads.
void** global_at(const std::uint8_t* code, std::size_t displacement, std::size_t next) {
  return reinterpret_cast<void**>(const_cast<std::uint8_t*>(code + next) +
                                  read<std::int32_t>(code, displacement));
}

void* queue_of_entity_system() {
  void* entity_system = entity_system_slot ? *entity_system_slot : nullptr;
  return entity_system ? static_cast<std::uint8_t*>(entity_system) + kEntitySystemQueue : nullptr;
}

void* runtime_prefabs() {
  void* prefab_system = prefab_system_slot ? *prefab_system_slot : nullptr;
  return prefab_system ? read<void*>(prefab_system, kRuntimePrefabs) : nullptr;
}

// The prefab template of a phase of the home or camp cauldron, looked up once.
void* template_of(bool camp, std::uint8_t phase) {
  const auto key = std::make_pair(camp, phase);
  const auto known = templates.find(key);
  if (known != templates.end()) {
    return known->second;
  }
  void* owner = library_owner();
  void* systems = owner ? read<void*>(owner, kLibraryOwnerSystems) : nullptr;
  void* library = systems ? read<void*>(systems, kSystemsLibrary) : nullptr;
  if (!library) {
    return nullptr;
  }
  // The name is the game's std::string (builds it: 32 bytes, data or inline text,
  // size, capacity); the lookup frees it. A single pointer here let the build write past it.
  GameString name;
  void* built = make_name(&name, camp ? kCampCategory : kHomeCategory);
  void* prefab = lookup(library, built, phase);
  if (prefab) {
    templates[key] = prefab;
  }
  return prefab;
}

// The game's swaps of a cauldron the server holds take the server's template.
char __fastcall hooked_queue(void* queue, const Swap* swap) {
  if (swap && queue == queue_of_entity_system()) {
    const auto found = held.find(swap->entity);
    if (found != held.end() && found->second != swap->prefab) {
      Swap server = *swap;
      server.prefab = found->second;
      return original_queue(queue, &server);
    }
  }
  return original_queue(queue, swap);
}

}  // namespace

bool install(const Natives& given) {
  natives = given;
  const auto* code = static_cast<const std::uint8_t*>(natives.binding);
  if (!code) {
    log_line("POTS SetPrefabPhaseById is missing");
    return false;
  }
  entity_system_slot = global_at(code, kEntitySystemDisplacement, kEntitySystemNext);
  prefab_system_slot = global_at(code, kPrefabSystemDisplacement, kPrefabSystemNext);
  find_prefab = reinterpret_cast<Find>(call_target(code, kFindCall));
  library_owner = reinterpret_cast<LibraryOwner>(call_target(code, kLibraryOwnerCall));
  make_name = reinterpret_cast<MakeName>(call_target(code, kNameCall));
  lookup = reinterpret_cast<Lookup>(call_target(code, kLookupCall));
  void* queue = call_target(code, kQueueCall);
  if (!find_prefab || !library_owner || !make_name || !lookup || !queue) {
    log_line("POTS SetPrefabPhaseById is not as expected");
    return false;
  }
  if (MH_CreateHook(queue, reinterpret_cast<void*>(&hooked_queue),
                    reinterpret_cast<void**>(&original_queue)) != MH_OK ||
      MH_EnableHook(queue) != MH_OK) {
    original_queue = nullptr;
    log_line("POTS could not hook the prefab phase queue %p", queue);
    return false;
  }
  return true;
}

bool show(std::uint32_t entity, bool camp, std::uint8_t phase) {
  void* queue = queue_of_entity_system();
  void* manager = runtime_prefabs();
  if (!original_queue || !queue || !manager || entity == 0) {
    log_line("POTS cannot show entity %u (queue %p, manager %p)", entity, queue, manager);
    return false;
  }
  // Not a runtime prefab: the binding refuses it too.
  void* set = static_cast<std::uint8_t*>(manager) + kRuntimePrefabSet;
  if (find_prefab(set, &entity) == read<void*>(set, 0)) {
    log_line("POTS entity %u is not a runtime prefab", entity);
    return false;
  }
  void* prefab = template_of(camp, phase);
  if (!prefab) {
    log_line("POTS no prefab for phase %u of the %s cauldron", unsigned(phase),
             camp ? "camp" : "home");
    return false;
  }
  const auto found = held.find(entity);
  if (found != held.end() && found->second == prefab) {
    return true;
  }
  held[entity] = prefab;
  Swap swap;
  swap.manager = manager;
  swap.entity = entity;
  swap.prefab = prefab;
  // The queue does the swap at once when it can (and answers false), else later.
  original_queue(queue, &swap);
  return true;
}

void release_all() {
  held.clear();
  // The next level has its own library.
  templates.clear();
}

}  // namespace kcds::runtime::cookpot
