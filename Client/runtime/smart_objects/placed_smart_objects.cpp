#include "placed_smart_objects.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "../props/prop_entity.hpp"
#include "MinHook.h"

namespace kcds::runtime::smart_objects {
namespace {

using kcd2s::integration::SmartObjectOrder;

// The prefab system's prefab manager, and the manager's instances by anchor entity id (an MSVC
// std::map: its head node at + 8; a node's left + 0, parent + 8, right + 16, is-head flag + 25,
// key + 32, value + 40).
constexpr std::size_t kPrefabManager = 0x150;
constexpr std::size_t kInstances = 8;
constexpr std::size_t kNodeLeft = 0;
constexpr std::size_t kNodeParent = 8;
constexpr std::size_t kNodeRight = 16;
constexpr std::size_t kNodeIsHead = 25;
constexpr std::size_t kNodeKey = 32;
constexpr std::size_t kNodeValue = 40;
// An instance's template, entity ids and brushes.
constexpr std::size_t kInstanceTemplate = 0;
constexpr std::size_t kInstanceEntities = 64;
constexpr std::size_t kInstanceBrushes = 88;
// A template's brush records (+ 24 to + 32, 112 bytes each) and a record's render flags (+ 16),
// which the build gives the brush. A template's RndFlags of more than 32 bits
// ("180040040408", the dice table's table) is read as all bits set: hidden and without physics.
// Such a record gets the flags of a solid piece of furniture that casts shadows instead.
constexpr std::size_t kTemplateBrushes = 24;
constexpr std::size_t kBrushRecordSize = 112;
constexpr std::size_t kBrushRecordFlags = 16;
constexpr std::uint32_t kUnreadFlags = 0xFFFFFFFFu;
constexpr std::uint32_t kFurnitureFlags = 0x40000008u;  // casts shadows
// CEntity: its id.
constexpr std::size_t kEntityId = 12;
// A point with nothing to draw (a BasicEntity draws the default pyramid).
constexpr char kAnchorClass[] = "TagPoint";
// IEntitySystem slot 38: an entity spawned from a template record (system, record, spawn params,
// out id, flag); the params' EntityGuid at + 8.
constexpr std::size_t kSpawnFromRecord = 38;
constexpr std::size_t kParamsGuid = 8;
// The params' world position and rotation (x, y, z, w).
constexpr std::size_t kParamsPosition = 80;
constexpr std::size_t kParamsRotation = 92;
constexpr std::uint64_t kGuidTag = 0x6b63ull << 48;  // "kc"

using SpawnPrefab = std::int64_t(__fastcall*)(void* manager, const void* template_guid,
                                              std::uint32_t anchor, int max_spawn, char now);
using ClearInstance = std::int64_t(__fastcall*)(void* instance);
using SpawnFromRecord = std::int64_t(__fastcall*)(void* system, void* record, void* params,
                                                  std::uint32_t* id, char flag);
SpawnFromRecord original_spawn_from_record = nullptr;
// While one of ours is built: its id and the place of the next entity, else id 0; the order,
// for its transform and corrections.
std::uint32_t building = 0;
std::uint32_t next_place = 0;
const SmartObjectOrder* building_order = nullptr;

// v turned by the quaternion q (x, y, z, w).
void turn_vector(const float q[4], const float v[3], float out[3]) {
  const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
  const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
  const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
  out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
  out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
  out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

// a then b turned by a: a * b (x, y, z, w).
void multiply(const float a[4], const float b[4], float out[4]) {
  out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
  out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
  out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
  out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

// The world transform of an entity of the build the order corrects, written into its params.
void correct(const SmartObjectOrder& order, std::uint32_t place, std::uint8_t* params) {
  for (const auto& correction : order.corrections) {
    if (correction.place != place) {
      continue;
    }
    float offset[3] = {};
    turn_vector(order.rotation, correction.at, offset);
    const float at[3] = {order.position[0] + offset[0], order.position[1] + offset[1],
                         order.position[2] + offset[2]};
    float turn[4] = {};
    multiply(order.rotation, correction.turn, turn);
    std::memcpy(params + kParamsPosition, at, sizeof(at));
    std::memcpy(params + kParamsRotation, turn, sizeof(turn));
    return;
  }
}

std::int64_t __fastcall hooked_spawn_from_record(void* system, void* record, void* params,
                                                 std::uint32_t* id, char flag) {
  if (building && params) {
    const std::uint32_t place = next_place++;
    const std::uint64_t guid = smart_object_guid(building, place);
    std::memcpy(static_cast<std::uint8_t*>(params) + kParamsGuid, &guid, sizeof(guid));
    if (building_order) {
      correct(*building_order, place, static_cast<std::uint8_t*>(params));
    }
  }
  return original_spawn_from_record(system, record, params, id, flag);
}

bool hook_spawn_from_record(void* entity_system) {
  void* target = entity_system ? (*static_cast<void***>(entity_system))[kSpawnFromRecord] : nullptr;
  return target &&
         MH_CreateHook(target, reinterpret_cast<void*>(&hooked_spawn_from_record),
                       reinterpret_cast<void**>(&original_spawn_from_record)) == MH_OK &&
         MH_EnableHook(target) == MH_OK;
}

Natives natives;
Logger log_line = nullptr;
bool installed = false;

struct Placed {
  std::string anchor;
  std::uint32_t anchor_id = 0;
};
// The smart objects placed, by the server's id.
std::map<std::uint32_t, Placed> placed;

template <class T>
T read(const void* base, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
  return value;
}

void* manager() {
  void* system = natives.prefab_system ? *natives.prefab_system : nullptr;
  return system ? read<void*>(system, kPrefabManager) : nullptr;
}

// The instance the manager built at that anchor entity, or null.
void* instance_of(std::uint32_t anchor_id) {
  void* prefabs = manager();
  void* head = prefabs ? read<void*>(prefabs, kInstances) : nullptr;
  void* node = head ? read<void*>(head, kNodeParent) : nullptr;
  for (int depth = 0; node && depth < 64 && !read<std::uint8_t>(node, kNodeIsHead); ++depth) {
    const std::uint32_t key = read<std::uint32_t>(node, kNodeKey);
    if (key == anchor_id) {
      return read<void*>(node, kNodeValue);
    }
    node = read<void*>(node, anchor_id < key ? kNodeLeft : kNodeRight);
  }
  return nullptr;
}

// The number of items of a vector at `offset` of an instance, each `size` bytes.
std::size_t count_of(void* instance, std::size_t offset, std::size_t size) {
  const auto begin = read<std::uintptr_t>(instance, offset);
  const auto end = read<std::uintptr_t>(instance, offset + 8);
  return begin && end >= begin ? (end - begin) / size : 0;
}

// Gives a template's brush records whose flags could not be read the furniture's; true when one
// was.
bool mend_brush_flags(void* instance) {
  void* source = instance ? read<void*>(instance, kInstanceTemplate) : nullptr;
  if (!source) {
    return false;
  }
  auto* record = read<std::uint8_t*>(source, kTemplateBrushes);
  auto* end = read<std::uint8_t*>(source, kTemplateBrushes + 8);
  bool mended = false;
  for (std::size_t count = 0; record && record < end && count < 1024; ++count) {
    if (read<std::uint32_t>(record, kBrushRecordFlags) == kUnreadFlags) {
      std::memcpy(record + kBrushRecordFlags, &kFurnitureFlags, sizeof(kFurnitureFlags));
      mended = true;
    }
    record += kBrushRecordSize;
  }
  return mended;
}

void take_down(std::uint32_t id) {
  const auto found = placed.find(id);
  if (found == placed.end()) {
    return;
  }
  void* instance = instance_of(found->second.anchor_id);
  if (instance) {
    reinterpret_cast<ClearInstance>(natives.clear_instance)(instance);
  }
  props::entity::remove(found->second.anchor);
  if (log_line) {
    log_line("SMARTOBJECT %u taken down (instance %u)", id, unsigned(instance != nullptr));
  }
  placed.erase(found);
}

void place(const SmartObjectOrder& order) {
  const std::string anchor = "kcds_so_" + std::to_string(order.id);
  void* entity =
      props::entity::spawn_class_with(anchor, kAnchorClass, order.position, order.rotation, {});
  const std::uint32_t anchor_id = entity ? read<std::uint32_t>(entity, kEntityId) : 0;
  void* prefabs = manager();
  const auto build = [&]() {
    building = order.id;
    building_order = &order;
    next_place = 0;
    reinterpret_cast<SpawnPrefab>(natives.spawn_prefab)(prefabs, order.template_guid.data(),
                                                        anchor_id, 0, 1);
    building = 0;
    building_order = nullptr;
  };
  if (anchor_id && prefabs) {
    build();
  }
  void* instance = anchor_id ? instance_of(anchor_id) : nullptr;
  // A brush built hidden (its template's flags unread) is built again with mended flags; the
  // manager takes the first build down itself.
  if (instance && mend_brush_flags(instance)) {
    build();
    instance = instance_of(anchor_id);
    if (log_line) {
      log_line("SMARTOBJECT %u built again: a brush's flags were mended", order.id);
    }
  }
  if (entity) {
    placed[order.id] = Placed{anchor, anchor_id};
  }
  // The keys its entities have (entity slot 2), which every client should share.
  std::string keys;
  if (instance && natives.entity_system) {
    using GetEntity = void*(__fastcall*)(void*, std::uint32_t);
    using EntityKey = std::uint64_t(__fastcall*)(void*);
    const auto* ids = read<const std::uint32_t*>(instance, kInstanceEntities);
    const std::size_t count = count_of(instance, kInstanceEntities, sizeof(std::uint32_t));
    for (std::size_t i = 0; ids && i < count && i < 16; ++i) {
      void* entity =
          (*static_cast<GetEntity**>(natives.entity_system))[14](natives.entity_system, ids[i]);
      const std::uint64_t key = entity ? (*static_cast<EntityKey**>(entity))[2](entity) : 0;
      char text[24] = {};
      std::snprintf(text, sizeof(text), "%s%llx", keys.empty() ? "" : ",",
                    static_cast<unsigned long long>(key));
      keys += text;
    }
  }
  if (log_line) {
    log_line("SMARTOBJECT %u keys %s", order.id, keys.c_str());
    log_line("SMARTOBJECT %u placed at %.2f %.2f %.2f: anchor %u, %zu entities, %zu brushes",
             order.id, order.position[0], order.position[1], order.position[2], anchor_id,
             instance ? count_of(instance, kInstanceEntities, sizeof(std::uint32_t)) : 0,
             instance ? count_of(instance, kInstanceBrushes, sizeof(void*)) : 0);
  }
}

}  // namespace

std::uint64_t smart_object_guid(std::uint32_t id, std::uint32_t place) {
  return kGuidTag | (std::uint64_t(id) << 16) | (place & 0xFFFF);
}

bool install(const Natives& found, Logger logger) {
  natives = found;
  log_line = logger;
  installed = natives.spawn_prefab && natives.clear_instance && natives.prefab_system &&
              hook_spawn_from_record(natives.entity_system);
  return installed;
}

void apply(const std::vector<SmartObjectOrder>& orders) {
  if (!installed) {
    return;
  }
  for (const SmartObjectOrder& order : orders) {
    take_down(order.id);
    if (!order.remove) {
      place(order);
    }
  }
}

void reset() {
  while (!placed.empty()) {
    take_down(placed.begin()->first);
  }
}

}  // namespace kcds::runtime::smart_objects
