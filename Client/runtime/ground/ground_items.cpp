#include "ground_items.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "MinHook.h"

#include "kcd2s/integration/world_items.hpp"

#include "../inventory/stock_projection.hpp"
#include "../props/prop_entity.hpp"

namespace kcds::runtime::ground {
namespace {

using ItemDescInit = void*(__fastcall*)(void* desc, const void* guid, int amount, float health);
using ItemDescFree = void(__fastcall*)(void* desc);
using InventoryAdd = void*(__fastcall*)(void* inventory, void* desc, unsigned reason, char flag);
using InventoryRemove = void*(__fastcall*)(void* inventory, void* item, unsigned reason,
                                           unsigned amount);
using Lay = void*(__fastcall*)(void* item, void* function, const float* position,
                               const float* rotation);
using PushFunction = void*(__fastcall*)(void* function, const float* impulse);
using FunctionFree = void(__fastcall*)(void* function);
using ItemByHandle = void*(__fastcall*)(void* registry, const std::uint64_t* handle);
using ScriptOnUsed = void*(__fastcall*)(void* bind, void* handler, unsigned user);
using HandlerCall = std::uint64_t(__fastcall*)(void* handler);
using DropUnits = char(__fastcall*)(void* owner, std::uint64_t handle, unsigned amount, void* a4,
                                    void** dropped);
using ItemOnUsed = char(__fastcall*)(void* item, unsigned user);
using Getter = void*(__fastcall*)(void* object);
using ItemOfEntity = void*(__fastcall*)(void* items, std::uint32_t entity);
// The framework's item system, and its item of an entity (as asks).
constexpr std::size_t kFrameworkItemSystem = 208 / 8;
constexpr std::size_t kItemSystemItemOfEntity = 168 / 8;

// Services + 224: the item services; their + 232 the inventory list, which holds the world's
// inventory at + 0x100070.
constexpr std::size_t kServicesItems = 224;
constexpr std::size_t kItemsInventories = 232;
constexpr std::size_t kWorldInventory = 0x100070;
// ItemDesc + 144: the quality the item is made in (stock_projection.cpp).
constexpr std::size_t kDescQuality = 144;
// Inventory change reasons: 4 is the script CreateItem's, 6 a removal by script.
constexpr unsigned kAddReason = 4;
constexpr unsigned kRemoveReason = 6;
constexpr std::size_t kItemAmount = 0x50;
constexpr std::size_t kItemHandle = 0x30;
constexpr std::size_t kRegistryItems = 0x168;
// An item's pickable (C_PickableItem) once it lies in the world, and the pickable's entity id.
constexpr std::size_t kItemPickable = 0x88;
constexpr std::size_t kPickableEntityId = 0x20;
// The inventory that holds an item (+ 0x98, else + 0x90; as the carry's pick-up asks).
constexpr std::size_t kItemHolder = 0x98;
constexpr std::size_t kItemOwner = 0x90;
// The script function handler: + 16 its self's entity id, + 88 returns nothing (as the bind does
// for an item it cannot find).
constexpr std::size_t kHandlerEntity = 16 / 8;
constexpr std::size_t kHandlerEndFunction = 88 / 8;
// The item's own OnUsed (user entity id): the pick-up the script bind runs (calls it at
// vtable + 0x2B8).
constexpr std::size_t kItemOnUsed = 0x2B8 / 8;
// An item's class (+ 0x48) and the class's GUID (+ 8).
constexpr std::size_t kItemClass = 0x48;
constexpr std::size_t kClassGuid = 8;
// A held pick-up the server never answered is let go after this long.
constexpr auto kWorldPickupWait = std::chrono::seconds(5);
constexpr auto kSweepPeriod = std::chrono::seconds(2);
// A stack that is not resting is looked at this often; it came to rest once it moved
// less than kSettleStep for kSettleStill, or after kSettleLongest whatever it does.
constexpr auto kSettleLook = std::chrono::milliseconds(100);
constexpr auto kSettleStill = std::chrono::milliseconds(500);
constexpr auto kSettleLongest = std::chrono::seconds(20);
constexpr float kSettleStep = 0.01f;
// The rests reported at most per kRestPeriod: the server takes 64 of a client's script messages a
// second, its resource events among them.
constexpr std::size_t kRestsPerPeriod = 2;
constexpr auto kRestPeriod = std::chrono::milliseconds(100);
// A laid stack the server moved farther than this is laid again where the server has it.
constexpr float kRelayDistance = 0.02f;

Natives natives;
Logger log_line = nullptr;
bool installed = false;
// The stacks this client laid: network id -> the game's item handle.
std::unordered_map<std::uint32_t, std::uint64_t> laid;
// Where the server had each laid stack when this client laid it.
std::unordered_map<std::uint32_t, Stack> laid_as;
// The laid stacks that were not resting: where their entity was at the last look, and
// since when it has not moved.
struct Settling {
  float last[3] = {};
  std::chrono::steady_clock::time_point still_since;
  std::chrono::steady_clock::time_point since;
};
std::unordered_map<std::uint32_t, Settling> settling;
std::chrono::steady_clock::time_point settled_look_at;
std::vector<Rest> rests;
std::chrono::steady_clock::time_point rests_sent_at;
// Stacks whose item a carry took over, until they leave the view, and their items' handles.
std::unordered_set<std::uint32_t> claimed;
std::unordered_set<std::uint64_t> claimed_handles;
// The game's own placing of an item into the world while it runs (Natives::place_in_world).
using PlaceInWorld = void(__fastcall*)(void* item, void* holder, int flags);
PlaceInWorld original_place = nullptr;
// Set while the game places an item that is not one of this module's stacks; this thread only.
thread_local bool placing_runtime_item = false;
ScriptOnUsed original_on_used = nullptr;
// The pickable entities made (level_item_spawned), for the sweeps.
std::mutex level_mutex;
std::unordered_set<std::uint32_t> level_entities;
std::mutex pickups_mutex;
std::vector<std::uint32_t> pickups;
DropUnits original_drop = nullptr;
std::vector<Drop> drops;
// The level's items asked of the server: key -> the item's handle and the player who asked.
struct HeldPickup {
  std::uint32_t entity = 0;
  unsigned user = 0;
  std::chrono::steady_clock::time_point at;
};
std::unordered_map<std::string, HeldPickup> held_pickups;
std::vector<std::string> world_pickups;
// The level's items taken up, and the classes among them (a sweep looks only at those).
std::unordered_set<std::string> taken_keys;
std::unordered_set<std::string> taken_classes;
// The server turned the level's items off: every one goes.
bool level_items_off = false;
// The level's items this client hid (taken up by someone, or the items turned off): entity -> true
// when only the switch hides it.
std::unordered_map<std::uint32_t, bool> hidden_level_items;
// The places a script took the level's items from (World.removeLevelItems): the
// centre, radius and class (its 32 hex digits as class_text_of writes them, '' for any), and the
// entities hidden for them.
struct RemovedArea {
  float center[3] = {0, 0, 0};
  float radius = 0;
  std::string item;
};
std::vector<RemovedArea> removed_areas;
std::unordered_set<std::uint32_t> removed_hidden;
// The looks of the piles hidden with them: a pile's look is an entity of its prefab, which the
// level gives no GUID (a camp's stack of bowls, its entity "bowl" showing
// bowls_lentil_4.cgf, stayed with its items hidden), drawn again once the place is no longer
// listed. Only an entity within kPileReach of an item the place took, at the pile's own place: the
// table, rack or bench under the item stays.
std::unordered_set<std::uint32_t> removed_props;
constexpr float kPileReach = 0.15f;
std::chrono::steady_clock::time_point swept_at;
bool sweep_due = false;

template <class Value>
Value at(const void* object, std::size_t offset) {
  Value value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

// The game's item of a handle, or null when there is none any more.
void* item_of(std::uint64_t handle) {
  void* registry = natives.item_registry ? *natives.item_registry : nullptr;
  if (!registry) {
    return nullptr;
  }
  return reinterpret_cast<ItemByHandle>(natives.item_by_handle)(
      static_cast<std::uint8_t*>(registry) + kRegistryItems, &handle);
}

void* world_inventory() {
  void* services = natives.services ? natives.services() : nullptr;
  void* items = services ? at<void*>(services, kServicesItems) : nullptr;
  void* inventories = items ? at<void*>(items, kItemsInventories) : nullptr;
  return inventories ? static_cast<std::uint8_t*>(inventories) + kWorldInventory : nullptr;
}

// A pickable's item (C_PickableItem + 0x58, as its use asks).
constexpr std::size_t kPickableItem = 0x58;

// The pickable (C_PickableItem) of an entity, or null: the item system's item of the entity.
void* pickable_of_entity(std::uint32_t entity) {
  void* framework = natives.framework ? natives.framework() : nullptr;
  void* items =
      framework ? (*static_cast<Getter**>(framework))[kFrameworkItemSystem](framework) : nullptr;
  return items ? (*static_cast<ItemOfEntity**>(items))[kItemSystemItemOfEntity](items, entity)
               : nullptr;
}

// The item a pickable entity lays, or null.
void* item_of_entity(std::uint32_t entity) {
  void* pickable = pickable_of_entity(entity);
  return pickable ? at<void*>(pickable, kPickableItem) : nullptr;
}

// An item a pickable entity lays: one the level placed lies loose, no inventory holds it (+ 0x98;
// its owner, + 0x90, is not an inventory, live: a tavern's tankard); one the game dropped lies in
// the world's inventory.
bool lies_in_level(void* item) {
  if (!item) {
    return false;
  }
  return lies_in_world(item) || !at<void*>(item, kItemHolder);
}

// The 32 hex digits of an item's class as scripts see it, or "".
std::string class_text_of(void* item) {
  void* item_class = item ? at<void*>(item, kItemClass) : nullptr;
  if (!item_class) {
    return {};
  }
  const auto guid = at<std::array<std::uint8_t, 16>>(item_class, kClassGuid);
  const float origin[3] = {};
  return kcd2s::integration::world_item_key(guid.data(), origin).substr(0, 32);
}

// The key of the level's item an entity lays (world_items.hpp), or "" when it has no class or
// place.
std::string key_of(std::uint32_t entity, void* item) {
  void* item_class = item ? at<void*>(item, kItemClass) : nullptr;
  float position[3] = {};
  if (!item_class || !natives.entity_position || !natives.entity_position(entity, position)) {
    return {};
  }
  const auto guid = at<std::array<std::uint8_t, 16>>(item_class, kClassGuid);
  return kcd2s::integration::world_item_key(guid.data(), position);
}

// The stack a pickable entity lays, or 0.
std::uint32_t stack_of_entity(std::uint32_t entity) {
  for (const auto& [id, handle] : laid) {
    void* item = item_of(handle);
    void* pickable = item ? at<void*>(item, kItemPickable) : nullptr;
    if (pickable && at<std::uint32_t>(pickable, kPickableEntityId) == entity) {
      return id;
    }
  }
  return 0;
}

char __fastcall drop_units(void* owner, std::uint64_t handle, unsigned amount, void* a4,
                           void** dropped) {
  std::string row;
  std::uint64_t revision = 0;
  if (amount > 0 && inventory::row_of(handle, row, revision)) {
    {
      std::lock_guard<std::mutex> lock(pickups_mutex);
      if (drops.size() < 16) {
        drops.push_back(Drop{row, amount});
      }
    }
    if (log_line) {
      log_line("GROUND drop of %u units of row %s asked of the server", amount, row.c_str());
    }
    if (dropped) {
      *dropped = nullptr;
    }
    return 0;
  }
  return original_drop(owner, handle, amount, a4, dropped);
}

void* __fastcall on_used(void* bind, void* handler, unsigned user) {
  const auto entity =
      std::uint32_t((*static_cast<HandlerCall**>(handler))[kHandlerEntity](handler));
  if (const std::uint32_t stack = entity ? stack_of_entity(entity) : 0) {
    {
      std::lock_guard<std::mutex> lock(pickups_mutex);
      if (pickups.size() < 16) {
        pickups.push_back(stack);
      }
    }
    if (log_line) {
      log_line("GROUND stack %u: pick-up asked of the server", stack);
    }
    return reinterpret_cast<void*>(
        (*static_cast<HandlerCall**>(handler))[kHandlerEndFunction](handler));
  }
  // One of the level's own items: the server decides who takes it up.
  void* item = entity ? item_of_entity(entity) : nullptr;
  const std::string key = item && lies_in_level(item) ? key_of(entity, item) : std::string();
  if (!key.empty()) {
    if (!taken_keys.count(key) && !held_pickups.count(key)) {
      std::lock_guard<std::mutex> lock(pickups_mutex);
      if (world_pickups.size() < 16) {
        world_pickups.push_back(key);
        held_pickups[key] = HeldPickup{entity, user, std::chrono::steady_clock::now()};
      }
    }
    if (log_line) {
      log_line("GROUND level item %s: pick-up asked of the server", key.c_str());
    }
    return reinterpret_cast<void*>(
        (*static_cast<HandlerCall**>(handler))[kHandlerEndFunction](handler));
  }
  return original_on_used(bind, handler, user);
}

}  // namespace

// Whether an item is one of the stacks this module laid or a carry took over.
bool own_item(void* item) {
  const auto handle = at<std::uint64_t>(item, kItemHandle);
  if (claimed_handles.count(handle)) {
    return true;
  }
  for (const auto& [id, laid_handle] : laid) {
    if (laid_handle == handle) {
      return true;
    }
  }
  return false;
}

void __fastcall place_in_world(void* item, void* holder, int flags) {
  const bool previous = placing_runtime_item;
  placing_runtime_item = item && !own_item(item);
  original_place(item, holder, flags);
  placing_runtime_item = previous;
}

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  installed = natives.services && natives.item_desc_init && natives.item_desc_free &&
              natives.inventory_add && natives.inventory_remove && natives.lay &&
              natives.push_function && natives.function_free && natives.item_by_handle &&
              natives.item_registry && natives.script_on_used && natives.drop;
  if (installed) {
    installed = MH_CreateHook(natives.script_on_used, reinterpret_cast<void*>(on_used),
                              reinterpret_cast<void**>(&original_on_used)) == MH_OK &&
                MH_EnableHook(natives.script_on_used) == MH_OK &&
                MH_CreateHook(natives.drop, reinterpret_cast<void*>(drop_units),
                              reinterpret_cast<void**>(&original_drop)) == MH_OK &&
                MH_EnableHook(natives.drop) == MH_OK;
  }
  if (installed && natives.place_in_world) {
    installed = MH_CreateHook(natives.place_in_world, reinterpret_cast<void*>(place_in_world),
                              reinterpret_cast<void**>(&original_place)) == MH_OK &&
                MH_EnableHook(natives.place_in_world) == MH_OK;
  }
  return installed;
}

bool refuses_runtime_item() {
  return placing_runtime_item;
}

// How far a falling stack leans toward its push (radians).
constexpr float kLean = 0.21f;

// rotation (x, y, z, w) leant by kLean toward the horizontal direction `angle`: turned about the
// horizontal axis across that direction, in world space.
void lean(float* rotation, float angle) {
  const float half = kLean * 0.5f;
  const float s = std::sin(half);
  // The axis (-sin, cos, 0) tips the top toward (cos, sin).
  const float l[4] = {-std::sin(angle) * s, std::cos(angle) * s, 0.0f, std::cos(half)};
  const float r[4] = {rotation[0], rotation[1], rotation[2], rotation[3]};
  rotation[0] = l[3] * r[0] + l[0] * r[3] + l[1] * r[2] - l[2] * r[1];
  rotation[1] = l[3] * r[1] - l[0] * r[2] + l[1] * r[3] + l[2] * r[0];
  rotation[2] = l[3] * r[2] + l[0] * r[1] - l[1] * r[0] + l[2] * r[3];
  rotation[3] = l[3] * r[3] - l[0] * r[0] - l[1] * r[1] - l[2] * r[2];
}

void* lay(const Stack& stack) {
  void* inventory = installed ? world_inventory() : nullptr;
  if (!inventory || stack.amount == 0) {
    return nullptr;
  }
  alignas(16) std::uint8_t desc[512] = {};
  // The game reads the GUID with an aligned 16-byte load (a fault otherwise, live).
  alignas(16) std::array<std::uint8_t, 16> guid = stack.item;
  reinterpret_cast<ItemDescInit>(natives.item_desc_init)(desc, guid.data(), int(stack.amount),
                                                         stack.health);
  if (stack.quality > 0) {
    const std::uint32_t quality = stack.quality;
    std::memcpy(desc + kDescQuality, &quality, sizeof(quality));
  }
  void* item =
      reinterpret_cast<InventoryAdd>(natives.inventory_add)(inventory, desc, kAddReason, 0);
  reinterpret_cast<ItemDescFree>(natives.item_desc_free)(desc);
  if (!item) {
    return nullptr;
  }
  // The game's own push: as its drop pushes a dropped item, sideways with the units'
  // weight; none for a stack at rest. The push alone did not tip a sword laid upright (it stood
  // balanced on its point): a falling stack also leans kLean toward the push, as no real
  // drop is quite upright, and gravity lays it down. The same on every client (the seed).
  alignas(16) std::uint8_t function[64] = {};
  float impulse[3] = {0, 0, 0};
  float rotation[4] = {stack.rotation[0], stack.rotation[1], stack.rotation[2], stack.rotation[3]};
  if (stack.push && natives.item_weight) {
    using Weight = float(__fastcall*)(void* item);
    const float weight = std::max(reinterpret_cast<Weight>(natives.item_weight)(item), 0.1f);
    const float angle = float(stack.push_seed % 360u) * 0.017453292f;
    impulse[0] = std::cos(angle) * weight;
    impulse[1] = std::sin(angle) * weight;
    lean(rotation, angle);
  }
  reinterpret_cast<PushFunction>(natives.push_function)(function, impulse);
  reinterpret_cast<Lay>(natives.lay)(item, function, stack.position, rotation);
  reinterpret_cast<FunctionFree>(natives.function_free)(function);
  return item;
}

bool take_away(void* item) {
  void* inventory = installed ? world_inventory() : nullptr;
  if (!inventory || !item) {
    return false;
  }
  reinterpret_cast<InventoryRemove>(natives.inventory_remove)(inventory, item, kRemoveReason,
                                                              at<std::uint32_t>(item, kItemAmount));
  return true;
}

namespace {

float distance(const float a[3], const float b[3]) {
  return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

// Whether a laid stack the server now has elsewhere (where some game saw it come to rest, issue)
// is to be laid again there: not when this client's own copy lies there already.
bool moved_by_server(std::uint32_t id, std::uint64_t handle, const Stack& stack) {
  const auto as = laid_as.find(id);
  if (as == laid_as.end() || distance(as->second.position, stack.position) <= kRelayDistance) {
    return false;
  }
  float here[3] = {};
  const std::uint32_t entity = entity_of(item_of(handle));
  if (entity && natives.entity_position && natives.entity_position(entity, here) &&
      distance(here, stack.position) <= kRelayDistance) {
    as->second = stack;
    return false;
  }
  return true;
}

// The laid stacks held without a body whose entity has not lost it yet, since when.
std::unordered_map<std::uint32_t, std::chrono::steady_clock::time_point> pinning;
// How long a pinned stack's entity is waited for.
constexpr auto kPinLongest = std::chrono::seconds(3);

// Takes the body of the pinned stacks' entities away, as soon as the game made them, so an arrow
// stuck in a target stays in it.
void pin_laid() {
  const auto now = std::chrono::steady_clock::now();
  for (auto it = pinning.begin(); it != pinning.end();) {
    const auto found = laid.find(it->first);
    const std::uint32_t entity = found != laid.end() ? entity_of(item_of(found->second)) : 0;
    const bool pinned = entity && natives.unphysicalize && natives.unphysicalize(entity);
    if (pinned || found == laid.end() || now - it->second >= kPinLongest) {
      if (log_line && found != laid.end()) {
        log_line("GROUND stack %u %s", it->first, pinned ? "pinned" : "could not be pinned");
      }
      it = pinning.erase(it);
    } else {
      ++it;
    }
  }
}

// Looks at the laid stacks that are not resting: one whose entity stopped moving came
// to rest, and where it lies is reported.
void watch_settling() {
  const auto now = std::chrono::steady_clock::now();
  if (settling.empty() || now - settled_look_at < kSettleLook) {
    return;
  }
  settled_look_at = now;
  for (auto it = settling.begin(); it != settling.end();) {
    Settling& watch = it->second;
    const auto found = laid.find(it->first);
    const std::uint32_t entity = found != laid.end() ? entity_of(item_of(found->second)) : 0;
    Rest rest;
    rest.id = it->first;
    const bool seen =
        entity && natives.entity_pose && natives.entity_pose(entity, rest.position, rest.rotation);
    if (!seen) {
      it = now - watch.since >= kSettleLongest ? settling.erase(it) : std::next(it);
      continue;
    }
    if (distance(watch.last, rest.position) > kSettleStep) {
      std::memcpy(watch.last, rest.position, sizeof(watch.last));
      watch.still_since = now;
    }
    if (now - watch.still_since < kSettleStill && now - watch.since < kSettleLongest) {
      ++it;
      continue;
    }
    rests.push_back(rest);
    if (log_line) {
      log_line("GROUND stack %u came to rest at %.2f %.2f %.2f", rest.id, rest.position[0],
               rest.position[1], rest.position[2]);
    }
    it = settling.erase(it);
  }
}

}  // namespace

void sync(const std::vector<Seen>& seen) {
  if (!installed) {
    return;
  }
  std::unordered_set<std::uint32_t> in_view;
  for (const Seen& stack : seen) {
    in_view.insert(stack.id);
    if (claimed.count(stack.id)) {
      continue;
    }
    const auto found = laid.find(stack.id);
    const bool moved = found != laid.end();
    if (moved) {
      if (stack.resting) {
        settling.erase(stack.id);
      }
      if (!moved_by_server(stack.id, found->second, stack.stack)) {
        continue;
      }
      void* item = item_of(found->second);
      if (item) {
        take_away(item);
      }
      laid.erase(found);
      laid_as.erase(stack.id);
      settling.erase(stack.id);
    }
    Stack laying = stack.stack;
    // One not at rest falls as the game's drop lets it (the same push on every client).
    laying.push = !stack.resting && !stack.stack.pinned;
    laying.push_seed = stack.id * 2654435761u;
    void* item = lay(laying);
    if (item) {
      laid[stack.id] = at<std::uint64_t>(item, kItemHandle);
      laid_as[stack.id] = stack.stack;
      if (stack.stack.pinned) {
        pinning[stack.id] = std::chrono::steady_clock::now();
      }
      if (!stack.resting) {
        Settling watch;
        std::memcpy(watch.last, stack.stack.position, sizeof(watch.last));
        watch.since = std::chrono::steady_clock::now();
        watch.still_since = watch.since;
        settling[stack.id] = watch;
      }
    }
    if (log_line) {
      log_line("GROUND stack %u x%u laid at %.2f %.2f %.2f%s: %s", stack.id, stack.stack.amount,
               stack.stack.position[0], stack.stack.position[1], stack.stack.position[2],
               moved ? " (moved by the server)" : "", item ? "yes" : "no");
    }
  }
  for (auto it = laid.begin(); it != laid.end();) {
    if (in_view.count(it->first)) {
      ++it;
      continue;
    }
    void* item = item_of(it->second);
    if (item) {
      take_away(item);
    }
    if (log_line) {
      log_line("GROUND stack %u taken away%s", it->first, item ? "" : " (the world had let it go)");
    }
    laid_as.erase(it->first);
    settling.erase(it->first);
    it = laid.erase(it);
  }
  for (auto it = claimed.begin(); it != claimed.end();) {
    it = in_view.count(*it) ? std::next(it) : claimed.erase(it);
  }
  watch_settling();
  pin_laid();
}

std::vector<Rest> take_rests() {
  const auto now = std::chrono::steady_clock::now();
  if (rests.empty() || now - rests_sent_at < kRestPeriod) {
    return {};
  }
  rests_sent_at = now;
  const std::size_t count = std::min(rests.size(), kRestsPerPeriod);
  std::vector<Rest> taken(rests.begin(), rests.begin() + count);
  rests.erase(rests.begin(), rests.begin() + count);
  return taken;
}

void* claim(std::uint32_t stack) {
  auto found = laid.find(stack);
  if (found == laid.end()) {
    return nullptr;
  }
  void* item = item_of(found->second);
  claimed_handles.insert(found->second);
  laid.erase(found);
  laid_as.erase(stack);
  settling.erase(stack);
  claimed.insert(stack);
  return item;
}

void unclaim(std::uint32_t stack, void* item) {
  if (claimed.erase(stack) && item) {
    claimed_handles.erase(at<std::uint64_t>(item, kItemHandle));
    laid[stack] = at<std::uint64_t>(item, kItemHandle);
  }
}

std::uint32_t entity_of(void* item) {
  void* pickable = item ? at<void*>(item, kItemPickable) : nullptr;
  return pickable ? at<std::uint32_t>(pickable, kPickableEntityId) : 0;
}

bool lies_in_world(void* item) {
  if (!installed || !item) {
    return false;
  }
  void* holder = at<void*>(item, kItemHolder);
  if (!holder) {
    holder = at<void*>(item, kItemOwner);
  }
  return holder && holder == world_inventory();
}

std::uint32_t stack_of(void* item) {
  if (!item) {
    return 0;
  }
  const auto handle = at<std::uint64_t>(item, kItemHandle);
  for (const auto& [id, laid_handle] : laid) {
    if (laid_handle == handle) {
      return id;
    }
  }
  return 0;
}

void* level_item_of(std::uint32_t entity) {
  void* item = installed && entity ? item_of_entity(entity) : nullptr;
  return item && lies_in_level(item) && !stack_of(item) ? item : nullptr;
}

std::vector<std::uint32_t> take_pickups() {
  std::lock_guard<std::mutex> lock(pickups_mutex);
  return std::exchange(pickups, {});
}

std::vector<std::string> take_world_pickups() {
  std::lock_guard<std::mutex> lock(pickups_mutex);
  return std::exchange(world_pickups, {});
}

void rule_world_pickups(const std::vector<std::pair<std::string, bool>>& verdicts) {
  for (const auto& [key, granted] : verdicts) {
    const auto found = held_pickups.find(key);
    if (found == held_pickups.end()) {
      continue;
    }
    const HeldPickup held = found->second;
    held_pickups.erase(found);
    // The game's own pick-up, as the script bind runs it: the pickable's OnUsed.
    void* pickable = granted ? pickable_of_entity(held.entity) : nullptr;
    void* item = pickable ? at<void*>(pickable, kPickableItem) : nullptr;
    const bool taken = item && lies_in_level(item) &&
                       reinterpret_cast<ItemOnUsed>((*static_cast<void***>(pickable))[kItemOnUsed])(
                           pickable, held.user) != 0;
    if (log_line) {
      log_line("GROUND level item %s: %s", key.c_str(),
               !granted ? "the server refused it"
               : taken  ? "taken up"
                        : "no longer here");
    }
  }
  const auto now = std::chrono::steady_clock::now();
  for (auto it = held_pickups.begin(); it != held_pickups.end();) {
    it = now - it->second.at > kWorldPickupWait ? held_pickups.erase(it) : std::next(it);
  }
}

void world_taken(bool full, const std::vector<std::string>& keys) {
  if (full) {
    taken_keys.clear();
    taken_classes.clear();
  }
  for (const std::string& key : keys) {
    taken_keys.insert(key);
    taken_classes.insert(key.substr(0, 32));
  }
  sweep_due = true;
}

void set_level_items_enabled(bool enabled) {
  level_items_off = !enabled;
  sweep_due = true;
}

void reset_world() {
  level_items_off = false;
  hidden_level_items.clear();
  removed_hidden.clear();
  removed_props.clear();
  taken_keys.clear();
  taken_classes.clear();
  held_pickups.clear();
  std::lock_guard<std::mutex> lock(pickups_mutex);
  world_pickups.clear();
}

void set_removed_areas(const std::vector<kcd2s::integration::RemovedItemArea>& areas) {
  static const char* digits = "0123456789abcdef";
  removed_areas.clear();
  for (const auto& area : areas) {
    RemovedArea made;
    std::memcpy(made.center, area.center, sizeof(made.center));
    made.radius = area.radius;
    const bool any = std::all_of(area.item.begin(), area.item.end(),
                                 [](std::uint8_t byte) { return byte == 0; });
    if (!any) {
      for (const std::uint8_t byte : area.item) {
        made.item += digits[byte >> 4];
        made.item += digits[byte & 15];
      }
    }
    removed_areas.push_back(made);
  }
  sweep_due = true;
}

// Whether a level item of that class lying there is in a place a script took the items from.
bool in_removed_area(const float position[3], const std::string& item) {
  for (const RemovedArea& area : removed_areas) {
    const float dx = position[0] - area.center[0];
    const float dy = position[1] - area.center[1];
    const float dz = position[2] - area.center[2];
    const bool inside = dx * dx + dy * dy + dz * dz <= area.radius * area.radius;
    if (inside && (area.item.empty() || area.item == item)) {
      return true;
    }
  }
  return false;
}

// The pile looks of the places a script took the level's items from: hidden, and drawn again once
// their items are back.
static void sweep_removed_props() {
  namespace entity = kcds::runtime::props::entity;
  std::vector<std::array<float, 3>> taken;
  std::unordered_set<std::uint32_t> items(removed_hidden.begin(), removed_hidden.end());
  // With the level's items off (World.setLevelItemsEnabled), their piles' looks go too.
  if (level_items_off) {
    for (const auto& [id, for_switch] : hidden_level_items) {
      if (for_switch) {
        items.insert(id);
      }
    }
  }
  for (const std::uint32_t id : items) {
    std::array<float, 3> at{};
    if (natives.entity_position && natives.entity_position(id, at.data())) {
      taken.push_back(at);
    }
  }
  std::unordered_set<std::uint32_t> now;
  if (!taken.empty()) {
    entity::each_entity([&](void* each) {
      const std::uint32_t id = entity::id_of(each);
      if (items.count(id)) {
        return;
      }
      float position[3];
      entity::world_position(each, position);
      for (const auto& at : taken) {
        const float dx = position[0] - at[0];
        const float dy = position[1] - at[1];
        const float dz = position[2] - at[2];
        if (dx * dx + dy * dy + dz * dz <= kPileReach * kPileReach) {
          now.insert(id);
          return;
        }
      }
    });
  }
  for (const std::uint32_t id : now) {
    if (!removed_props.count(id)) {
      natives.hide_entity(id, true);
    }
  }
  for (const std::uint32_t id : removed_props) {
    if (!now.count(id)) {
      natives.hide_entity(id, false);
    }
  }
  if (now != removed_props && log_line) {
    log_line("GROUND pile looks hidden with removed items: %zu", now.size());
  }
  removed_props = std::move(now);
}

void level_item_spawned(std::uint32_t entity) {
  std::lock_guard<std::mutex> lock(level_mutex);
  level_entities.insert(entity);
}

void sweep_world() {
  const auto now = std::chrono::steady_clock::now();
  const bool nothing = taken_keys.empty() && !level_items_off && hidden_level_items.empty() &&
                       removed_areas.empty() && removed_hidden.empty() && removed_props.empty();
  if (!installed || nothing || (!sweep_due && now - swept_at < kSweepPeriod)) {
    return;
  }
  sweep_due = false;
  swept_at = now;
  std::vector<std::uint32_t> entities;
  {
    std::lock_guard<std::mutex> lock(level_mutex);
    entities.assign(level_entities.begin(), level_entities.end());
  }
  unsigned gone = 0;
  unsigned back = 0;
  std::vector<std::uint32_t> forget;
  for (const std::uint32_t entity : entities) {
    float position[3] = {};
    if (!natives.entity_position || !natives.entity_position(entity, position)) {
      forget.push_back(entity);  // the entity went (its part of the level unloaded)
      hidden_level_items.erase(entity);
      continue;
    }
    // A script's removed places (World.removeLevelItems): hidden while inside one, back after.
    if (removed_hidden.count(entity)) {
      void* item = item_of_entity(entity);
      if (!item || !in_removed_area(position, class_text_of(item))) {
        natives.hide_entity(entity, false);
        removed_hidden.erase(entity);
        ++back;
      }
      continue;
    }
    if (!removed_areas.empty() && natives.hide_entity) {
      void* item = item_of_entity(entity);
      if (item && lies_in_level(item) && !stack_of(item) &&
          in_removed_area(position, class_text_of(item))) {
        natives.hide_entity(entity, true);
        removed_hidden.insert(entity);
        ++gone;
        continue;
      }
    }
    const auto hidden = hidden_level_items.find(entity);
    // Turned on again: an item hidden only for the switch comes back.
    if (hidden != hidden_level_items.end()) {
      if (!level_items_off && hidden->second) {
        natives.hide_entity(entity, false);
        hidden_level_items.erase(hidden);
        ++back;
      }
      continue;
    }
    void* item = item_of_entity(entity);
    if (!item || !lies_in_level(item) || stack_of(item)) {
      continue;
    }
    // Only an item of a class among the taken ones is placed; with the level's items off, every
    // one is.
    if (!level_items_off && !taken_classes.count(class_text_of(item))) {
      continue;
    }
    const std::string key = key_of(entity, item);
    const bool taken = !key.empty() && taken_keys.count(key);
    if ((taken || level_items_off) && !held_pickups.count(key)) {
      // One the game dropped leaves the world's inventory. One the level placed is hidden: its
      // entity removed, the game made it anew from the level's item at once.
      if (lies_in_world(item)) {
        take_away(item);
        forget.push_back(entity);
      } else if (natives.hide_entity) {
        natives.hide_entity(entity, true);
        hidden_level_items[entity] = !taken;
      }
      ++gone;
    }
  }
  {
    std::lock_guard<std::mutex> lock(level_mutex);
    for (const std::uint32_t entity : forget) {
      level_entities.erase(entity);
    }
  }
  sweep_removed_props();
  if ((gone || back) && log_line) {
    log_line("GROUND level items: %u gone from this world, %u back", gone, back);
  }
}

std::vector<Drop> take_drops() {
  std::lock_guard<std::mutex> lock(pickups_mutex);
  return std::exchange(drops, {});
}

void ask_drop(const std::string& row, std::uint32_t amount) {
  if (row.empty() || amount == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(pickups_mutex);
  if (drops.size() < 16) {
    drops.push_back(Drop{row, amount});
  }
}

void* item_of_handle(std::uint64_t handle) {
  return installed && handle ? item_of(handle) : nullptr;
}

}  // namespace kcds::runtime::ground
