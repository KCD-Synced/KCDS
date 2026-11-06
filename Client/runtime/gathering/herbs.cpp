#include "herbs.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "MinHook.h"

namespace kcds::runtime::gathering {
namespace {

namespace wire = kcd2s::integration;

// The game services' environment module and its respawn manager (slot 11), and the object that
// works out a player's pick radius (slot 18) and a herb's amount (slot 17).
constexpr std::size_t kServicesEnvironment = 312;
constexpr std::size_t kEnvironmentManager = 11;
constexpr std::size_t kServicesItems = 304;
constexpr std::size_t kItemsCalculator = 152;
constexpr std::size_t kCalculatorAmount = 17;
constexpr std::size_t kCalculatorRadius = 18;
constexpr std::size_t kActorSoul = 1640;
// The manager's pick under the crosshair, and its records of picks (a hashed list: its end node
// at + 8; a node's record at + 16).
constexpr std::size_t kManagerPending = 96;
constexpr std::size_t kManagerRecords = 160;
constexpr std::size_t kRecordsEnd = 8;
constexpr std::size_t kNodeRecord = 16;
// C_HerbGathering: the herb's item class, the amount factor, how many plants the pick took, and
// its end (slot 5) as the game runs it after giving the herb.
constexpr std::size_t kHerbItem = 112;
constexpr std::size_t kHerbFactor = 128;
constexpr std::size_t kHerbPlants = 152;
constexpr std::size_t kHerbEnd = 5;
// The manager's grid of plants (+ 152): a hashed list of cells (its end node at + 8 of the list at
// + 24; a cell's plants at + 24 to + 32 of its node, 48 bytes each: where at + 8, the kind at + 20,
// shown at + 21).
constexpr std::size_t kManagerGrid = 152;
constexpr std::size_t kGridCells = 24;
constexpr std::size_t kCellsEnd = 8;
constexpr std::size_t kCellPlantsBegin = 24;
constexpr std::size_t kCellPlantsEnd = 32;
constexpr std::size_t kPlantSize = 48;
constexpr std::size_t kPlantPosition = 8;
constexpr std::size_t kPlantKind = 20;
constexpr std::size_t kPlantShown = 21;
// A pick wider than this (a kind switched off map-wide) gathers its plants from the
// grid's own cells: the game walks every cell within the radius, empty or not.
constexpr float kWideRadius = 500.0f;
// A regrowth deadline that never comes.
constexpr std::uint64_t kNever = 0x7FFFFFFFFFFFFFFFull;
// Asks remembered at once, and how long one waits for the server's answer (milliseconds).
constexpr std::size_t kMaxPending = 16;
constexpr std::uint64_t kPendingLifetimeMs = 30000;

// The manager's pick: where (+ 8) and which kind (+ 20).
struct PickDesc {
  std::uint64_t head = 0;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  std::uint8_t kind = 0;
  std::uint8_t shown = 1;
  std::uint8_t pad[2] = {};
};
static_assert(sizeof(PickDesc) == 24);

// The manager's record of a pick, as its respawn reads it: where, the radius and the
// kind (+ 16).
struct PickRecord {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float radius = 0.0f;
  std::uint8_t kind = 0;
  std::uint8_t pad[15] = {};
};

using Pick = std::int32_t(__fastcall*)(void* manager, const PickDesc* desc, std::uint64_t deadline,
                                       float radius);
using Respawn = bool(__fastcall*)(void* manager, const PickRecord* record);
using EraseRecord = void*(__fastcall*)(void* records, void* node);
using Grant = std::int64_t(__fastcall*)(void* herb);
using Collect = void**(__fastcall*)(void* manager, const float* position, float radius,
                                    std::uint8_t kind, std::uint8_t shown);
using Reserve = void(__fastcall*)(void* begin, const std::uint64_t* count);
using GetManager = void*(__fastcall*)(void* environment);
using Radius = float(__fastcall*)(void* calculator, void* soul);
using Amount = std::uint32_t(__fastcall*)(void* calculator, void* soul, std::uint32_t units);
using End = std::int64_t(__fastcall*)(void* herb);

struct Picked {
  PickRecord record;
  std::uint64_t at = 0;
};

Natives natives;
Pick original_pick = nullptr;
Grant original_grant = nullptr;
Collect original_collect = nullptr;
Reserve reserve_collected = nullptr;
Respawn respawn_plants = nullptr;
EraseRecord erase_record = nullptr;
bool online = false;
std::uint32_t next_request = 1;
// The local player's last pick, waiting for its herb to be given.
std::optional<Picked> local_pick;
// request -> the pick asked about
std::map<std::uint32_t, Picked> pending;
std::vector<wire::GatherReport> to_send;
// The server's picks this game ran, by id.
std::map<std::uint32_t, PickRecord> applied;

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

template <class Fn>
Fn slot_of(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*static_cast<void***>(object))[index]);
}

void* manager() {
  void* services = natives.services ? natives.services() : nullptr;
  void* environment = services ? read<void*>(services, kServicesEnvironment) : nullptr;
  return environment ? slot_of<GetManager>(environment, kEnvironmentManager)(environment) : nullptr;
}

void* calculator() {
  void* services = natives.services ? natives.services() : nullptr;
  void* items = services ? read<void*>(services, kServicesItems) : nullptr;
  return items ? read<void*>(items, kItemsCalculator) : nullptr;
}

void* local_soul() {
  void* actor = natives.local_actor ? natives.local_actor() : nullptr;
  return actor ? read<void*>(actor, kActorSoul) : nullptr;
}

// The plants of a pick come back: the shown ones at once, and its records go, so plants that
// stream in later are shown too.
void grow_back(void* respawns, const PickRecord& record) {
  respawn_plants(respawns, &record);
  auto* records = static_cast<std::uint8_t*>(respawns) + kManagerRecords;
  void* end = read<void*>(records, kRecordsEnd);
  void* node = end ? read<void*>(end, 0) : nullptr;
  while (node && node != end) {
    const auto* kept =
        reinterpret_cast<const PickRecord*>(static_cast<std::uint8_t*>(node) + kNodeRecord);
    const float dx = kept->x - record.x;
    const float dy = kept->y - record.y;
    const float dz = kept->z - record.z;
    const bool same = kept->kind == record.kind && dx * dx + dy * dy + dz * dz < 0.01f &&
                      kept->radius >= record.radius - 0.01f &&
                      kept->radius <= record.radius + 0.01f;
    node = same ? erase_record(records, node) : read<void*>(node, 0);
  }
}

void bring_back(const PickRecord& record) {
  if (void* respawns = manager()) {
    grow_back(respawns, record);
  }
}

// The manager's pick. The local player's (the plant under the crosshair) gets the player's radius
// and a deadline that never comes, and is remembered for the herb it gives.
std::int32_t __fastcall hooked_pick(void* manager, const PickDesc* desc, std::uint64_t deadline,
                                    float radius) {
  const auto* pending_pick =
      reinterpret_cast<const PickDesc*>(static_cast<std::uint8_t*>(manager) + kManagerPending);
  if (!online || desc != pending_pick) {
    return original_pick(manager, desc, deadline, radius);
  }
  if (radius <= 0.0f) {
    void* calculating = calculator();
    void* soul = local_soul();
    if (calculating && soul) {
      radius = slot_of<Radius>(calculating, kCalculatorRadius)(calculating, soul);
    }
  }
  const std::int32_t taken = original_pick(manager, desc, kNever, radius);
  if (taken > 0) {
    Picked picked;
    picked.record.x = desc->x;
    picked.record.y = desc->y;
    picked.record.z = desc->z;
    picked.record.radius = radius;
    picked.record.kind = desc->kind;
    picked.at = GetTickCount64();
    local_pick = picked;
  }
  return taken;
}

// The manager's gathering of the plants of a kind within a radius (for its pick and its
// respawn) into its static list (begin, end, capacity). A wide pick goes through the grid's cells
// instead of every cell its radius covers; the list is the same.
void** __fastcall hooked_collect(void* manager, const float* position, float radius,
                                 std::uint8_t kind, std::uint8_t shown) {
  if (radius <= kWideRadius) {
    return original_collect(manager, position, radius, kind, shown);
  }
  // The game's own call (no radius) readies the list.
  void** list = original_collect(manager, position, 0.0f, kind, shown);
  void* grid = read<void*>(manager, kManagerGrid);
  if (!grid || !reserve_collected) {
    return list;
  }
  auto* cells = static_cast<std::uint8_t*>(grid) + kGridCells;
  void* end = read<void*>(cells, kCellsEnd);
  std::vector<void*> found;
  for (void* node = end ? read<void*>(end, 0) : nullptr; node && node != end;
       node = read<void*>(node, 0)) {
    auto* plant = read<std::uint8_t*>(node, kCellPlantsBegin);
    auto* last = read<std::uint8_t*>(node, kCellPlantsEnd);
    for (; plant && plant != last; plant += kPlantSize) {
      if (plant[kPlantKind] != kind || plant[kPlantShown] != shown) {
        continue;
      }
      const float dx = read<float>(plant, kPlantPosition) - position[0];
      const float dy = read<float>(plant, kPlantPosition + 4) - position[1];
      if (dx * dx + dy * dy <= radius * radius) {
        found.push_back(plant);
      }
    }
  }
  list[1] = list[0];
  const auto capacity =
      static_cast<std::uint64_t>(static_cast<void**>(list[2]) - static_cast<void**>(list[0]));
  if (found.size() > capacity) {
    const std::uint64_t count = found.size();
    reserve_collected(list[0], &count);
  }
  if (!found.empty()) {
    std::memcpy(list[0], found.data(), found.size() * sizeof(void*));
  }
  list[1] = static_cast<void**>(list[0]) + found.size();
  return list;
}

// The herb gathering's end, which gives the herb: in a server's world the server is asked instead
// and the minigame ends as the game ends it after giving.
std::int64_t __fastcall hooked_grant(void* herb) {
  const auto item_low = read<std::uint64_t>(herb, kHerbItem);
  const auto item_high = read<std::uint64_t>(herb, kHerbItem + 8);
  const auto plants = read<std::uint32_t>(herb, kHerbPlants);
  if (!online || (!item_low && !item_high) || plants == 0 || !local_pick) {
    return original_grant(herb);
  }
  void* calculating = calculator();
  void* soul = local_soul();
  const auto factor = read<std::uint32_t>(herb, kHerbFactor);
  const std::uint32_t amount =
      calculating && soul
          ? slot_of<Amount>(calculating, kCalculatorAmount)(calculating, soul, plants * factor)
          : 0;
  if (amount > 0 && pending.size() < kMaxPending) {
    wire::GatherReport report;
    report.request = next_request++;
    std::memcpy(report.item.data(), static_cast<std::uint8_t*>(herb) + kHerbItem,
                report.item.size());
    report.amount = amount;
    report.kind = local_pick->record.kind;
    report.position = {local_pick->record.x, local_pick->record.y, local_pick->record.z};
    report.radius = local_pick->record.radius;
    report.plants = plants;
    pending[report.request] = *local_pick;
    to_send.push_back(report);
    log_line("GATHER asked to keep a pick of kind %u (%u plants, %u units, request %u)",
             unsigned(report.kind), plants, amount, report.request);
  } else {
    // Nothing the server could give: the plants come back.
    bring_back(local_pick->record);
  }
  local_pick.reset();
  return slot_of<End>(herb, kHerbEnd)(herb);
}

template <class Fn>
bool hook(void* target, void* detour, Fn* original, const char* name) {
  if (!target) {
    log_line("GATHER %s is missing", name);
    return false;
  }
  if (MH_CreateHook(target, detour, reinterpret_cast<void**>(original)) != MH_OK ||
      MH_EnableHook(target) != MH_OK) {
    *original = nullptr;
    log_line("GATHER could not hook %s %p", name, target);
    return false;
  }
  return true;
}

}  // namespace

bool install(const Natives& given) {
  natives = given;
  respawn_plants = reinterpret_cast<Respawn>(natives.respawn);
  erase_record = reinterpret_cast<EraseRecord>(natives.erase_record);
  reserve_collected = reinterpret_cast<Reserve>(natives.reserve);
  if (!natives.services || !natives.local_actor || !respawn_plants || !erase_record) {
    log_line("GATHER natives are missing");
    return false;
  }
  if (natives.collect && reserve_collected) {
    hook(natives.collect, reinterpret_cast<void*>(&hooked_collect), &original_collect,
         "the respawn manager's gathering");
  }
  return hook(natives.pick, reinterpret_cast<void*>(&hooked_pick), &original_pick,
              "the respawn manager's pick") &&
         hook(natives.grant, reinterpret_cast<void*>(&hooked_grant), &original_grant,
              "the herb gathering's grant");
}

void set_online(bool now_online) {
  if (online && !now_online) {
    // A world left: the server's picks go with it, and their plants grow back, as do the picks
    // still waiting for an answer or a herb.
    for (const auto& [id, record] : applied) {
      bring_back(record);
    }
    for (const auto& [request, asked] : pending) {
      bring_back(asked.record);
    }
    if (local_pick) {
      bring_back(local_pick->record);
    }
    applied.clear();
    pending.clear();
    local_pick.reset();
  }
  online = now_online;
  // A pick the server never answered, or one whose herb never came: its plants come back.
  const std::uint64_t now = GetTickCount64();
  for (auto entry = pending.begin(); entry != pending.end();) {
    if (now - entry->second.at > kPendingLifetimeMs) {
      bring_back(entry->second.record);
      entry = pending.erase(entry);
    } else {
      ++entry;
    }
  }
  if (local_pick && now - local_pick->at > kPendingLifetimeMs) {
    bring_back(local_pick->record);
    local_pick.reset();
  }
}

std::vector<wire::GatherReport> take_reports() {
  return std::exchange(to_send, {});
}

void apply_verdict(const wire::GatherVerdict& verdict) {
  const auto asked = pending.find(verdict.request);
  if (asked == pending.end()) {
    return;
  }
  if (!verdict.granted) {
    bring_back(asked->second.record);
    log_line("GATHER the server refused pick request %u: the plants come back", verdict.request);
  }
  pending.erase(asked);
}

void apply_plants(const wire::GatherPlants& plants) {
  void* respawns = manager();
  if (!respawns || !original_pick) {
    return;
  }
  if (plants.full) {
    for (const auto& [id, record] : applied) {
      grow_back(respawns, record);
    }
    applied.clear();
  }
  for (const wire::GatherPick& pick : plants.picked) {
    if (applied.count(pick.id) != 0) {
      continue;
    }
    PickDesc desc;
    desc.x = pick.position[0];
    desc.y = pick.position[1];
    desc.z = pick.position[2];
    desc.kind = pick.kind;
    const std::int32_t hidden = original_pick(respawns, &desc, kNever, pick.radius);
    log_line("GATHER pick %u of kind %u hides %d plants", pick.id, pick.kind, hidden);
    PickRecord record;
    record.x = desc.x;
    record.y = desc.y;
    record.z = desc.z;
    record.radius = pick.radius;
    record.kind = pick.kind;
    applied[pick.id] = record;
  }
  for (const std::uint32_t id : plants.grown) {
    const auto found = applied.find(id);
    if (found != applied.end()) {
      grow_back(respawns, found->second);
      applied.erase(found);
      log_line("GATHER pick %u grew back", id);
    }
  }
}

long dump_plants(const wchar_t* path) {
  void* respawns = manager();
  void* grid = respawns ? read<void*>(respawns, kManagerGrid) : nullptr;
  if (!grid) {
    return -1;
  }
  FILE* out = nullptr;
  if (_wfopen_s(&out, path, L"w") != 0 || !out) {
    return -1;
  }
  auto* cells = static_cast<std::uint8_t*>(grid) + kGridCells;
  void* end = read<void*>(cells, kCellsEnd);
  long count = 0;
  for (void* node = end ? read<void*>(end, 0) : nullptr; node && node != end;
       node = read<void*>(node, 0)) {
    auto* plant = read<std::uint8_t*>(node, kCellPlantsBegin);
    auto* last = read<std::uint8_t*>(node, kCellPlantsEnd);
    for (; plant && plant != last; plant += kPlantSize) {
      std::fprintf(out, "%u,%.3f,%.3f,%.3f\n", unsigned(plant[kPlantKind]),
                   read<float>(plant, kPlantPosition), read<float>(plant, kPlantPosition + 4),
                   read<float>(plant, kPlantPosition + 8));
      ++count;
    }
  }
  std::fclose(out);
  return count;
}

}  // namespace kcds::runtime::gathering
