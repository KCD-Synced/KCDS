#include "player_buffs.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <map>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "MinHook.h"
#include "custom_buffs.hpp"

namespace kcds::runtime::buffs {
namespace {

namespace integration = kcd2s::integration;
using integration::BuffEntry;
using integration::Guid;
using Kind = integration::BuffOrder::Kind;

using GetManager = void*(__fastcall*)();
using Create = void**(__fastcall*)(void* manager, void** out, void* soul, const Guid* buff,
                                   void* source, const void* params);
using Definition = const std::uint8_t*(__fastcall*)(void* unused, const Guid* buff);
using AddBuff = void*(__fastcall*)(void* manager, void* soul, const Guid* buff, void* source,
                                   const void* params);
using RemoveInstance = bool(__fastcall*)(void* manager, const std::uint64_t* instance);
using RemoveAll = int(__fastcall*)(void* manager, void* soul, const Guid* buff);
using RemoveFamily = int(__fastcall*)(void* manager, void* soul, const std::int32_t* tag);
using Construct = void**(__fastcall*)(void* manager, void** out, void* soul, const void* definition,
                                      const void* original, void* init_params, const void* perk);

constexpr std::size_t kSoulBuffsBegin = 1352;
constexpr std::size_t kSoulBuffsEnd = 1360;
constexpr std::size_t kInstanceId = 8;
constexpr std::size_t kInstanceDefinition = 16;
constexpr std::size_t kDefinitionClass = 32;
constexpr std::size_t kAddSlot = 0;
constexpr std::size_t kRemoveInstanceSlot = 3;
constexpr std::size_t kRemoveAllSlot = 5;
constexpr std::size_t kRemoveFamilySlot = 6;
// A blocked buff is told at most this often per second.
constexpr unsigned kBlockedPerSecond = 2;
constexpr ULONGLONG kSecondMs = 1000;

Logger log_line = nullptr;
Natives natives;
Create original_create = nullptr;
void* local_soul = nullptr;
std::vector<std::uint8_t> claimed;
// Ids of the instances the server added, and when this client first saw each instance.
std::vector<std::uint64_t> server_instances;
std::map<std::uint64_t, ULONGLONG> first_seen;
bool server_adding = false;
// While add_defined() runs: the soul and the definition its create makes the instance of.
void* defined_soul = nullptr;
const void* defined_definition = nullptr;
std::vector<Guid> blocked;
std::map<Guid, std::vector<ULONGLONG>> blocked_at;

template <class T>
T field(const void* base, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
  return value;
}

template <class Fn>
Fn slot(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*static_cast<void***>(object))[index]);
}

bool contains(const std::vector<std::uint8_t>& classes, std::uint32_t id) {
  return std::find(classes.begin(), classes.end(), id) != classes.end();
}

bool status_class(std::uint32_t id) {
  const auto& status = integration::kStatusBuffClasses;
  return std::find(status.begin(), status.end(), id) != status.end();
}

// The class of the buff, or -1 when the game has no such buff.
int class_of(const Guid* buff) {
  const std::uint8_t* definition = reinterpret_cast<Definition>(natives.definition)(nullptr, buff);
  return definition ? int(field<std::uint32_t>(definition, kDefinitionClass)) : -1;
}

// Tells the server of a claimed buff the game tried to add, at most twice a second each.
void note_blocked(const Guid& buff) {
  const ULONGLONG now = GetTickCount64();
  auto& times = blocked_at[buff];
  times.erase(std::remove_if(times.begin(), times.end(),
                             [&](ULONGLONG at) { return now - at >= kSecondMs; }),
              times.end());
  if (times.size() >= kBlockedPerSecond) {
    return;
  }
  times.push_back(now);
  blocked.push_back(buff);
}

void** __fastcall hooked_create(void* manager, void** out, void* soul, const Guid* buff,
                                void* source, const void* params) {
  if (defined_definition && soul && soul == defined_soul && out && params) {
    // add_defined(): the instance is made of the caller's definition, as the game makes one of
    // a perk's override; the definition is its own original, so the instance has no override.
    const void* definition = std::exchange(defined_definition, nullptr);
    alignas(16) std::uint8_t perk[16];
    std::memcpy(perk, params, sizeof(perk));
    return reinterpret_cast<Construct>(natives.construct)(manager, out, soul, definition,
                                                          definition, source, perk);
  }
  if (!server_adding && !claimed.empty() && soul && soul == local_soul && buff && out) {
    const int buff_class = class_of(buff);
    if (buff_class >= 0 && contains(claimed, std::uint32_t(buff_class))) {
      note_blocked(*buff);
      *out = nullptr;
      return out;
    }
  }
  return original_create(manager, out, soul, buff, source, params);
}

void* manager() {
  return reinterpret_cast<GetManager>(natives.manager)();
}

void add(void* soul, const Guid& buff) {
  // The game loads GUIDs and parameters with aligned moves.
  alignas(16) const Guid id = buff;
  alignas(16) const std::uint8_t params[16] = {};
  void* buff_manager = manager();
  if (!buff_manager) {
    return;
  }
  server_adding = true;
  void* instance = slot<AddBuff>(buff_manager, kAddSlot)(buff_manager, soul, &id, nullptr, params);
  server_adding = false;
  if (instance) {
    server_instances.push_back(field<std::uint64_t>(instance, kInstanceId));
  }
}

void remove(void* soul, const integration::BuffOrder& order) {
  void* buff_manager = manager();
  if (!buff_manager) {
    return;
  }
  if (order.instances.empty()) {
    alignas(16) const Guid id = order.buff;
    slot<RemoveAll>(buff_manager, kRemoveAllSlot)(buff_manager, soul, &id);
    return;
  }
  for (const std::uint64_t instance : order.instances) {
    slot<RemoveInstance>(buff_manager, kRemoveInstanceSlot)(buff_manager, &instance);
  }
}

void clear(void* soul, std::uint8_t tag) {
  void* buff_manager = manager();
  if (!buff_manager) {
    return;
  }
  const std::int32_t family = tag;
  slot<RemoveFamily>(buff_manager, kRemoveFamilySlot)(buff_manager, soul, &family);
}

}  // namespace

bool install(const Natives& resolved, Logger logger) {
  log_line = logger;
  natives = resolved;
  if (!natives.manager || !natives.create || !natives.definition) {
    return false;
  }
  return MH_CreateHook(natives.create, reinterpret_cast<void*>(hooked_create),
                       reinterpret_cast<void**>(&original_create)) == MH_OK &&
         MH_EnableHook(natives.create) == MH_OK;
}

void tick(void* soul) {
  local_soul = soul;
  if (!soul) {
    claimed.clear();
    server_instances.clear();
    first_seen.clear();
    blocked.clear();
    blocked_at.clear();
  }
}

void apply(void* soul, const std::vector<integration::BuffOrder>& orders) {
  if (!soul) {
    return;
  }
  for (const auto& order : orders) {
    switch (order.kind) {
      case Kind::Add:
        add(soul, order.buff);
        break;
      case Kind::Remove:
        remove(soul, order);
        break;
      case Kind::Clear:
        clear(soul, order.tag);
        break;
      case Kind::Claim:
        claimed = order.classes;
        break;
    }
    if (log_line) {
      log_line("BUFFS order %u buff=%s instances=%zu tag=%u classes=%zu", unsigned(order.kind),
               integration::guid_text(order.buff).c_str(), order.instances.size(),
               unsigned(order.tag), order.classes.size());
    }
  }
}

std::vector<Carried> read(void* soul) {
  std::vector<Carried> carried;
  if (!soul) {
    return carried;
  }
  const auto* begin = field<void* const*>(soul, kSoulBuffsBegin);
  const auto* end = field<void* const*>(soul, kSoulBuffsEnd);
  if (!begin || end < begin) {
    return carried;
  }
  const ULONGLONG now = GetTickCount64();
  std::map<std::uint64_t, ULONGLONG> seen;
  for (auto it = begin; it != end && carried.size() < integration::kMaxBuffs; ++it) {
    const void* instance = *it;
    const void* definition = instance ? field<const void*>(instance, kInstanceDefinition) : nullptr;
    // The resources' own buffs are the server's to report (custom_buffs.hpp).
    if (!definition || custom::owns(definition)) {
      continue;
    }
    const std::uint64_t id = field<std::uint64_t>(instance, kInstanceId);
    const bool server =
        std::find(server_instances.begin(), server_instances.end(), id) != server_instances.end();
    if (!server && !status_class(field<std::uint32_t>(definition, kDefinitionClass))) {
      continue;
    }
    Carried buff;
    std::memcpy(buff.entry.buff.data(), definition, buff.entry.buff.size());
    buff.entry.instance = id;
    buff.entry.server = server;
    const auto known = first_seen.find(id);
    const ULONGLONG since = known != first_seen.end() ? known->second : now;
    seen[id] = since;
    buff.age = double(now - since) / double(kSecondMs);
    carried.push_back(buff);
  }
  first_seen = std::move(seen);
  // The server's instances that are gone stay gone.
  server_instances.erase(std::remove_if(server_instances.begin(), server_instances.end(),
                                        [&](std::uint64_t id) { return !first_seen.count(id); }),
                         server_instances.end());
  return carried;
}

const std::uint8_t* definition_of(const Guid& buff) {
  if (!natives.definition) {
    return nullptr;
  }
  alignas(16) const Guid id = buff;
  return reinterpret_cast<Definition>(natives.definition)(nullptr, &id);
}

void* add_defined(void* soul, const Guid& buff, const void* definition) {
  if (!soul || !definition || !natives.construct) {
    return nullptr;
  }
  alignas(16) const Guid id = buff;
  alignas(16) const std::uint8_t params[16] = {};
  void* buff_manager = manager();
  if (!buff_manager) {
    return nullptr;
  }
  defined_soul = soul;
  defined_definition = definition;
  void* instance = slot<AddBuff>(buff_manager, kAddSlot)(buff_manager, soul, &id, nullptr, params);
  // The add reaches the create unless the game turned it down before.
  defined_soul = nullptr;
  defined_definition = nullptr;
  return instance;
}

std::uint64_t add_to_soul(void* soul, const Guid& buff) {
  // The game loads GUIDs and parameters with aligned moves.
  alignas(16) const Guid id = buff;
  alignas(16) const std::uint8_t params[16] = {};
  void* buff_manager = manager();
  if (!soul || !buff_manager) {
    return 0;
  }
  void* instance = slot<AddBuff>(buff_manager, kAddSlot)(buff_manager, soul, &id, nullptr, params);
  return instance ? field<std::uint64_t>(instance, kInstanceId) : 0;
}

bool remove_instance(std::uint64_t instance) {
  void* buff_manager = manager();
  if (!buff_manager) {
    return false;
  }
  return slot<RemoveInstance>(buff_manager, kRemoveInstanceSlot)(buff_manager, &instance);
}

std::vector<std::uint64_t> instance_ids(void* soul) {
  std::vector<std::uint64_t> ids;
  if (!soul) {
    return ids;
  }
  const auto* begin = field<void* const*>(soul, kSoulBuffsBegin);
  const auto* end = field<void* const*>(soul, kSoulBuffsEnd);
  if (!begin || end < begin) {
    return ids;
  }
  for (auto it = begin; it != end; ++it) {
    if (*it) {
      ids.push_back(field<std::uint64_t>(*it, kInstanceId));
    }
  }
  return ids;
}

std::vector<Guid> take_blocked() {
  if (log_line && !blocked.empty()) {
    log_line("BUFFS claimed buffs turned down: %zu (first %s)", blocked.size(),
             integration::guid_text(blocked.front()).c_str());
  }
  return std::exchange(blocked, {});
}

}  // namespace kcds::runtime::buffs
