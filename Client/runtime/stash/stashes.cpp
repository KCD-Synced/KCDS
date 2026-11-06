#include "stashes.hpp"

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "MinHook.h"

#include "../inventory/stock_projection.hpp"
#include "../level/level_edits.hpp"
#include "../props/prop_entity.hpp"
#include "kcd2s/integration/level_edits.hpp"

namespace kcds::runtime::stash {
namespace {

namespace integration = kcd2s::integration;
using Kind = integration::InventoryReport::Kind;

// The game's own corpse container (no model of its own, the Loot prompt), and the chest a
// container that is not a body shows in its second slot.
constexpr char kStashClass[] = "StashCorpse";
constexpr char kChestModel[] = "objects/characters/assets/chest/chest_fancy_b.cdf";
constexpr int kChestSlot = 1;
// A chest's prompt while the crosshair is on it (a ball about the chest), and no further than the
// game's own use distance (Stash.fUseDistance 2.5 from the feet, about 3 from the eyes).
constexpr float kChestAimHeight = 0.35f;
constexpr float kChestAimRadius = 0.6f;
constexpr float kChestAimReach = 3.0f;
// Services + 224 the item services, their + 232 the inventories, whose + 40 is the registry the
// transfer screen finds an inventory by its id in (as does).
constexpr std::size_t kServicesItems = 224;
constexpr std::size_t kItemsInventories = 232;
constexpr std::size_t kInventoryRegistry = 40;
// The item services are the entity module (C_EntityModule): its + 16 is the actor script binding
// whose OpenItemTransferStore opens the transfer screen (makes it). A screen a script
// opens has no use to take the binding from.
constexpr std::size_t kModuleActorBinding = 16;
// An inventory's + 232: slot 0 of that part gives the address of the inventory's id (the
// registry compares it so).
constexpr std::size_t kInventoryIdentity = 232;
// CEntity's id; an item's handle and units.
constexpr std::size_t kEntityId = 12;
constexpr std::size_t kItemHandle = 0x30;
constexpr std::size_t kItemAmount = 0x50;
// The binding keeps the container whose screen is open at + 128 until the close callback.
constexpr std::size_t kBindingEntity = 128;
// The script function handler: slot 2 the id of the entity it was called on, slot 11 the end
// of the call.
constexpr std::size_t kHandlerThis = 2;
constexpr std::size_t kHandlerEnd = 11;
constexpr std::size_t kHandlerSlots = 16;
constexpr std::size_t kMaxReports = 64;
// The lock. CEntity: the script proxy (GetProxy slot 74, type 2) and its script table
// (slot 18). IScriptTable: Release slot 4, SetValueAny slot 7, GetValueAny slot 8; a
// ScriptAnyValue is 32 bytes, its type first and the value at + 8. C_LockBase keeps its entity
// at + 56; SetLocked reads a callback from its context at + 56 (zeros: none).
constexpr std::size_t kGetProxySlot = 74;
constexpr int kScriptProxy = 2;
constexpr std::size_t kScriptTableSlot = 18;
constexpr std::size_t kTableRelease = 4;
constexpr std::size_t kSetValueAnySlot = 7;
constexpr std::size_t kGetValueAnySlot = 8;
constexpr std::size_t kAnySize = 32;
constexpr std::size_t kAnyValue = 8;
constexpr int kAnyBool = 2;
constexpr int kAnyNumber = 4;
constexpr int kAnyString = 5;
constexpr int kAnyTable = 6;
// Lock.fanciness: a script's common or fancy lock; anything else the level's.
constexpr int kFancinessCommon = 1;
constexpr int kFancinessFancy = 2;
constexpr std::size_t kLockEntity = 56;
constexpr std::size_t kLockIdOffset = 0x28;
constexpr std::size_t kLockContextSize = 128;
// The lock-picking minigame keeps the entity id of the lock it works at + 40 (its start,
// sets it; its unlock finds the lock from it through).
constexpr std::size_t kMinigameTarget = 40;

using OpenTransfer = std::int64_t(__fastcall*)(void* binding, void* handler, int entity,
                                               std::uint64_t inventory, const char* filter,
                                               const char* header);
using TransferClosed = void(__fastcall*)(void* binding);
using TransferItem = char(__fastcall*)(void** pair, void* item, unsigned units);
using InventoryById = void*(__fastcall*)(void* registry, const std::uint64_t* id);
using InventoryIdentity = const std::uint64_t*(__fastcall*)(void* identity);
using HandlerCall = std::int64_t(__fastcall*)(void* handler);
using GetProxy = void*(__fastcall*)(void* entity, int type);
using GetScriptTable = void*(__fastcall*)(void* proxy);
using ReleaseTable = void(__fastcall*)(void* table);
using SetValueAny = void(__fastcall*)(void* table, const char* key, const void* value, bool chain);
using GetValueAny = bool(__fastcall*)(void* table, const char* key, void* value, bool chain);
using LockOfEntityId = void*(__fastcall*)(const void* holder);
using SetLocked = std::int64_t(__fastcall*)(void* lock, char locked, void* context);
using LockpickUnlock = std::int64_t(__fastcall*)(void* minigame, unsigned char success,
                                                 char immediate, void* result);

Natives natives;
Logger log_line = nullptr;
bool installed = false;
OpenTransfer original_open = nullptr;
TransferClosed original_closed = nullptr;
TransferItem original_item = nullptr;
LockpickUnlock original_lockpick_unlock = nullptr;

struct State {
  std::string name;
  std::uint32_t entity = 0;
  // An adopted level chest: the level's own entity, which this client never removes.
  bool level = false;
  // Where it stands; a body's follows the body.
  float position[3] = {};
  // A dead NPC's body: its entity, whose Rob opens this container, and its actor.
  std::uint32_t body_entity = 0;
  void* wearer = nullptr;
  // What its script's use passed to the transfer screen, kept while the server decides: the
  // entity used (the container's own, or a dead NPC's body) and that entity's inventory.
  void* binding = nullptr;
  std::uint32_t used_entity = 0;
  std::uint64_t inventory_id = 0;
  std::string filter;
  std::string header;
  bool waiting = false;
  // What its prompt says about the stock, the server's word (a script's chest).
  bool empty = false;
  bool empty_known = false;
  // A virtual stash's stand-in: a container without a model at the local player's
  // feet, made when a script opens it and taken away with the next sync after it closes.
  bool stand_in = false;
  // A chest of a script's (no body, no cart): it has the Stash class's prompts, see
  // give_chest_prompts.
  bool chest = false;
  bool aimed = false;
  // Its inventory and the server's rows by the handles of the items that show them.
  void* inventory = nullptr;
  std::unordered_map<std::uint64_t, std::string> rows;
  // The lock last put on the game's lock, once one was, and whether the player's
  // unlock of it was reported since (the server's answer is the next lock it sends).
  Lock lock;
  bool lock_applied = false;
  bool unlock_reported = false;
  // The lock-picking difficulty the game gave it before a script set one, restored
  // when the script gives it back; below 0 not read yet.
  float own_difficulty = -1.0f;
  // The lock the level gave it (Properties.Lock esLockFanciness, bLockFancinessOverride) before a
  // script chose one, restored when the script gives it back.
  bool own_fanciness_read = false;
  std::string own_fanciness;
  bool own_fanciness_override = false;
};

std::mutex reports_mutex;
std::vector<Report> reports;
// Network id -> container, and its entity's id -> network id.
std::unordered_map<std::uint32_t, State> stashes;
std::unordered_map<std::uint32_t, std::uint32_t> by_entity;
// The server's NPC bodies, whose own inventory the game's Rob never opens.
std::unordered_set<std::uint32_t> server_bodies;
// The container whose transfer screen is open, 0 for none; set while the screen is opened from
// here, so the hook lets that call through.
std::uint32_t open_stash = 0;
bool granting = false;

template <class Function>
Function slot(void* object, std::size_t index) {
  return (*static_cast<Function**>(object))[index];
}

template <class Value>
Value at(const void* object, std::size_t offset) {
  Value value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

void report(Kind kind, std::uint32_t stash, const std::string& row = {}, std::uint32_t units = 0) {
  std::lock_guard<std::mutex> lock(reports_mutex);
  if (reports.size() < kMaxReports) {
    reports.push_back(Report{kind, stash, row, units});
  }
}

// The player opened the lock of a container the server holds locked: told once, until the
// server sends its lock again.
void report_unlock(std::uint32_t id, State& state, integration::StashUnlockMethod method) {
  if (!state.lock.locked || state.unlock_reported) {
    return;
  }
  state.unlock_reported = true;
  {
    std::lock_guard<std::mutex> lock(reports_mutex);
    if (reports.size() < kMaxReports) {
      Report unlock;
      unlock.stash = id;
      unlock.unlock = true;
      unlock.method = method;
      reports.push_back(std::move(unlock));
    }
  }
  if (log_line) {
    log_line("STASH %u unlocked by %s: told the server", id,
             integration::stash_unlock_method_name(method));
  }
}

// The handler the screen is opened with from here: the call was made on the local player.
std::int64_t __fastcall handler_nothing(void*) {
  return 0;
}

std::int64_t __fastcall handler_this(void*) {
  void* entity = natives.local_entity ? natives.local_entity() : nullptr;
  return entity ? std::int64_t(at<std::uint32_t>(entity, kEntityId)) : 0;
}

struct OwnHandler {
  void* const* vtable;
};

void* const* own_handler_vtable() {
  static void* table[kHandlerSlots] = {};
  for (std::size_t index = 0; index < kHandlerSlots; ++index) {
    table[index] = reinterpret_cast<void*>(handler_nothing);
  }
  table[kHandlerThis] = reinterpret_cast<void*>(handler_this);
  return table;
}

void* inventory_of(std::uint64_t id) {
  void* services = natives.services ? natives.services() : nullptr;
  void* items = services ? at<void*>(services, kServicesItems) : nullptr;
  void* inventories = items ? at<void*>(items, kItemsInventories) : nullptr;
  if (!inventories) {
    return nullptr;
  }
  void* registry = static_cast<std::uint8_t*>(inventories) + kInventoryRegistry;
  return reinterpret_cast<InventoryById>(natives.inventory_by_id)(registry, &id);
}

void* actor_binding() {
  void* services = natives.services ? natives.services() : nullptr;
  void* module = services ? at<void*>(services, kServicesItems) : nullptr;
  return module ? at<void*>(module, kModuleActorBinding) : nullptr;
}

std::uint64_t inventory_id_of(void* inventory) {
  void* identity = static_cast<std::uint8_t*>(inventory) + kInventoryIdentity;
  const std::uint64_t* id = slot<InventoryIdentity>(identity, 0)(identity);
  return id ? *id : 0;
}

// A screen a script opens: what a use of the container would have passed, taken from
// its entity. False when the entity or its inventory is not there (yet).
bool prepare_script_open(State& state) {
  void* entity = props::entity::by_id(state.entity);
  void* inventory = entity ? inventory::inventory_of_entity(entity) : nullptr;
  void* binding = actor_binding();
  if (!inventory || !binding) {
    return false;
  }
  state.binding = binding;
  state.used_entity = state.entity;
  state.inventory = inventory;
  state.inventory_id = inventory_id_of(inventory);
  state.filter.clear();
  state.header.clear();
  state.waiting = true;
  return state.inventory_id != 0;
}

// The actor whose own inventory the container was opened on (a dead NPC's body), or none.
void* wearer_of(const State& state) {
  return state.body_entity && state.used_entity == state.body_entity ? state.wearer : nullptr;
}

// Empties a container's inventory: what it showed is the server's.
void forget_rows(State& state) {
  if (state.inventory) {
    inventory::fill_container(state.inventory, {}, wearer_of(state));
  }
  state.rows.clear();
}

std::int64_t __fastcall open_transfer(void* binding, void* handler, int entity,
                                      std::uint64_t inventory, const char* filter,
                                      const char* header) {
  const auto found = granting ? by_entity.end() : by_entity.find(std::uint32_t(entity));
  if (found == by_entity.end()) {
    if (!granting && server_bodies.count(std::uint32_t(entity))) {
      // A server NPC's body without a container of the server's: nothing to rob.
      if (log_line) {
        log_line("STASH rob of a server NPC's body 0x%x refused: no container", entity);
      }
      return slot<HandlerCall>(handler, kHandlerEnd)(handler);
    }
    return original_open(binding, handler, entity, inventory, filter, header);
  }
  State& state = stashes[found->second];
  state.binding = binding;
  state.used_entity = std::uint32_t(entity);
  if (state.inventory_id != inventory) {
    state.inventory = nullptr;
  }
  state.inventory_id = inventory;
  state.filter = filter ? filter : "";
  state.header = header ? header : "";
  state.waiting = true;
  // The game lets a container the server holds locked be used only by the key's holder (a pick
  // was reported when the minigame opened it): the key's unlock goes before the open.
  report_unlock(found->second, state, integration::StashUnlockMethod::Key);
  report(Kind::StashOpen, found->second);
  if (log_line) {
    log_line("STASH %u used: asked the server", found->second);
  }
  return slot<HandlerCall>(handler, kHandlerEnd)(handler);
}

void __fastcall transfer_closed(void* binding) {
  const auto entity = at<std::uint32_t>(binding, kBindingEntity);
  original_closed(binding);
  const auto found = by_entity.find(entity);
  if (found == by_entity.end() || found->second != open_stash) {
    return;
  }
  open_stash = 0;
  report(Kind::StashClose, found->second);
  forget_rows(stashes[found->second]);
  if (log_line) {
    log_line("STASH %u closed", found->second);
  }
}

// While one of ours is open, nothing moves in its screen by itself: an item it shows is taken,
// a unit of a server row of the player's is put.
char __fastcall transfer_item(void** pair, void* item, unsigned units) {
  if (open_stash == 0 || !item) {
    return original_item(pair, item, units);
  }
  State& state = stashes[open_stash];
  const auto handle = at<std::uint64_t>(item, kItemHandle);
  const std::uint32_t amount = units ? units : at<std::uint32_t>(item, kItemAmount);
  if (const auto shown = state.rows.find(handle); shown != state.rows.end()) {
    report(Kind::StashTake, open_stash, shown->second, amount);
    if (log_line) {
      log_line("STASH %u take %u of row %s asked", open_stash, amount, shown->second.c_str());
    }
    return 1;
  }
  std::string row;
  std::uint64_t revision = 0;
  if (inventory::row_of(handle, row, revision)) {
    report(Kind::StashPut, open_stash, row, amount);
    if (log_line) {
      log_line("STASH %u put %u of row %s asked", open_stash, amount, row.c_str());
    }
    return 1;
  }
  if (log_line) {
    log_line("STASH %u: an item nobody keeps stays where it is", open_stash);
  }
  return 1;
}

// The lock-picking minigame opened the lock it works at (it calls SetLocked itself): when that is
// one of ours the server holds locked, the pick is reported.
std::int64_t __fastcall lockpick_unlock(void* minigame, unsigned char success, char immediate,
                                        void* result) {
  const std::uint32_t target = minigame ? at<std::uint32_t>(minigame, kMinigameTarget) : 0;
  const std::int64_t done = original_lockpick_unlock(minigame, success, immediate, result);
  const auto found = by_entity.find(target);
  if (target != 0 && found != by_entity.end()) {
    State& state = stashes[found->second];
    if (state.entity == target) {
      report_unlock(found->second, state, integration::StashUnlockMethod::Pick);
    }
  }
  return done;
}

// A container that moved (a knocked-out player's body carried away) is placed where it is now, so
// the Loot prompt is on the body.
void follow(State& state, const Seen& container) {
  constexpr float kMoved = 0.1f;
  if (state.level) {
    return;  // a level chest stands where the level put it
  }
  const float dx = container.position[0] - state.position[0];
  const float dy = container.position[1] - state.position[1];
  const float dz = container.position[2] - state.position[2];
  if (dx * dx + dy * dy + dz * dz < kMoved * kMoved) {
    return;
  }
  void* entity = props::entity::find(state.name);
  if (!entity) {
    return;
  }
  const float angles[3] = {};
  props::entity::place(entity, container.position, angles);
  props::entity::rotate(entity, container.rotation);
  std::memcpy(state.position, container.position, sizeof(state.position));
}

// A dead NPC's body opens the container with its Rob (its body may be made anew).
void bind_body(std::uint32_t id, State& state, const Seen& container) {
  state.wearer = container.wearer;
  if (state.body_entity == container.body_entity) {
    return;
  }
  if (state.body_entity) {
    by_entity.erase(state.body_entity);
  }
  state.body_entity = container.body_entity;
  if (state.body_entity) {
    by_entity[state.body_entity] = id;
  }
}

// ---- The lock ---------------------------------------------------------------------

void* script_table_of(void* entity) {
  void* proxy = slot<GetProxy>(entity, kGetProxySlot)(entity, kScriptProxy);
  return proxy ? slot<GetScriptTable>(proxy, kScriptTableSlot)(proxy) : nullptr;
}

// The table a script value holds, with a reference the caller releases; null for none.
void* table_value(void* table, const char* key) {
  alignas(16) std::uint8_t value[kAnySize] = {};
  if (!table || !slot<GetValueAny>(table, kGetValueAnySlot)(table, key, value, false)) {
    return nullptr;
  }
  if (at<int>(value, 0) != kAnyTable) {
    return nullptr;
  }
  return at<void*>(value, kAnyValue);
}

void set_bool(void* table, const char* key, bool flag) {
  alignas(16) std::uint8_t value[kAnySize] = {};
  std::memcpy(value, &kAnyBool, sizeof(kAnyBool));
  value[kAnyValue] = flag ? 1 : 0;
  slot<SetValueAny>(table, kSetValueAnySlot)(table, key, value, false);
}

// A flag of a script table, or `fallback`.
bool bool_of(void* table, const char* key, bool fallback) {
  alignas(16) std::uint8_t value[kAnySize] = {};
  if (!table || !slot<GetValueAny>(table, kGetValueAnySlot)(table, key, value, false) ||
      at<int>(value, 0) != kAnyBool) {
    return fallback;
  }
  return value[kAnyValue] != 0;
}

// A text of a script table, or `fallback`.
std::string text_of(void* table, const char* key, const char* fallback) {
  alignas(16) std::uint8_t value[kAnySize] = {};
  if (!table || !slot<GetValueAny>(table, kGetValueAnySlot)(table, key, value, false) ||
      at<int>(value, 0) != kAnyString) {
    return fallback;
  }
  const char* text = at<const char*>(value, kAnyValue);
  return text ? std::string(text) : std::string(fallback);
}

// The script system copies the text.
// A number of a script table (the script system keeps numbers as floats), or `fallback`.
float number_of(void* table, const char* key, float fallback) {
  alignas(16) std::uint8_t value[kAnySize] = {};
  if (!table || !slot<GetValueAny>(table, kGetValueAnySlot)(table, key, value, false) ||
      at<int>(value, 0) != kAnyNumber) {
    return fallback;
  }
  return at<float>(value, kAnyValue);
}
// A script's chest is a StashCorpse, whose script says "Loot" from any side, as for a body. Its
// parent Stash's GetActions says the game's "Open" and asks the script's `interactive`, which
// aim_chests keeps on only while the player points at the chest.
void give_chest_prompts(void* entity) {
  void* script = script_table_of(entity);
  void* parent = script ? table_value(script, "__super") : nullptr;
  if (!parent) {
    if (log_line) {
      log_line("STASH chest: no parent script, it keeps the body's prompt");
    }
    return;
  }
  alignas(16) std::uint8_t actions[kAnySize] = {};
  if (slot<GetValueAny>(parent, kGetValueAnySlot)(parent, "GetActions", actions, false)) {
    slot<SetValueAny>(script, kSetValueAnySlot)(script, "GetActions", actions, false);
    set_bool(script, "interactive", false);
  }
  // The script takes itself for open once its use has run, and its prompt then says "Close", also
  // for a player the server refuses. This game's screen is the server's: the chest never does.
  alignas(16) std::uint8_t nothing[kAnySize] = {};
  if (slot<GetValueAny>(parent, kGetValueAnySlot)(parent, "UsesStealUiPrompt", nothing, false)) {
    slot<SetValueAny>(script, kSetValueAnySlot)(script, "OnOpen", nothing, false);
  }
  slot<ReleaseTable>(parent, kTableRelease)(parent);
}

// The prompt adds "(empty)" when HasPlayerVisibleItems answers anything but false, by the local
// inventory, which holds nothing until the server fills it at the open. The server's word decides:
// UsesStealUiPrompt answers false for a chest without an owner, IsUsable without a user a number.
void show_empty(std::uint32_t entity_id, State& state, bool empty) {
  void* entity = props::entity::by_id(entity_id);
  void* script = entity ? script_table_of(entity) : nullptr;
  void* parent = script ? table_value(script, "__super") : nullptr;
  if (!parent) {
    return;
  }
  alignas(16) std::uint8_t answer[kAnySize] = {};
  const char* source = empty ? "IsUsable" : "UsesStealUiPrompt";
  if (slot<GetValueAny>(parent, kGetValueAnySlot)(parent, source, answer, false)) {
    slot<SetValueAny>(script, kSetValueAnySlot)(script, "HasPlayerVisibleItems", answer, false);
    state.empty = empty;
    state.empty_known = true;
  }
  slot<ReleaseTable>(parent, kTableRelease)(parent);
}

void set_number(void* table, const char* key, float number) {
  alignas(16) std::uint8_t value[kAnySize] = {};
  std::memcpy(value, &kAnyNumber, sizeof(kAnyNumber));
  std::memcpy(value + kAnyValue, &number, sizeof(number));
  slot<SetValueAny>(table, kSetValueAnySlot)(table, key, value, false);
}
void set_text(void* table, const char* key, const char* text) {
  alignas(16) std::uint8_t value[kAnySize] = {};
  std::memcpy(value, &kAnyString, sizeof(kAnyString));
  std::memcpy(value + kAnyValue, &text, sizeof(text));
  slot<SetValueAny>(table, kSetValueAnySlot)(table, key, value, false);
}

// Sets the container's Properties.Lock (lock-picking, the key) and locks or unlocks the game's
// lock, whose SetLocked runs the script's own Lock or Unlock. False when the entity has no lock.
bool put_lock(void* entity, const Lock& lock, State& state) {
  if (!natives.lock_of_entity_id || !natives.set_locked) {
    return false;
  }
  // The lookup reads the entity id at + 0x28 of its argument (see stashes.hpp).
  struct IdHolder {
    std::uint8_t before[kLockIdOffset];
    std::uint32_t id;
    std::uint32_t after;
  };
  IdHolder holder{};
  holder.id = at<std::uint32_t>(entity, kEntityId);
  void* game_lock = reinterpret_cast<LockOfEntityId>(natives.lock_of_entity_id)(&holder);
  void* script = script_table_of(entity);
  if (!game_lock || at<void*>(game_lock, kLockEntity) != entity || !script) {
    return false;
  }
  void* properties = table_value(script, "Properties");
  void* lock_properties = table_value(properties, "Lock");
  if (lock_properties) {
    set_bool(lock_properties, "bCanLockPick", lock.canBePicked);
    set_text(lock_properties, "guidItemClassId", lock.key_class.c_str());
    // The lock-picking minigame reads the difficulty from here at its start (issue):
    // the script's, or the game's own again.
    if (state.own_difficulty < 0.0f) {
      state.own_difficulty = number_of(lock_properties, "fLockDifficulty", 0.5f);
    }
    set_number(lock_properties, "fLockDifficulty",
               lock.difficulty >= 0.0f ? lock.difficulty : state.own_difficulty);
    // Which lock it shows, read with the difficulty: the script's, or the level's.
    if (!state.own_fanciness_read) {
      state.own_fanciness = text_of(lock_properties, "esLockFanciness", "Common");
      state.own_fanciness_override = bool_of(lock_properties, "bLockFancinessOverride", false);
      state.own_fanciness_read = true;
    }
    if (lock.fanciness == kFancinessCommon || lock.fanciness == kFancinessFancy) {
      set_text(lock_properties, "esLockFanciness",
               lock.fanciness == kFancinessFancy ? "Fancy" : "Common");
      set_bool(lock_properties, "bLockFancinessOverride", true);
    } else {
      set_text(lock_properties, "esLockFanciness", state.own_fanciness.c_str());
      set_bool(lock_properties, "bLockFancinessOverride", state.own_fanciness_override);
    }
    slot<ReleaseTable>(lock_properties, kTableRelease)(lock_properties);
  }
  if (properties) {
    slot<ReleaseTable>(properties, kTableRelease)(properties);
  }
  alignas(16) std::uint8_t context[kLockContextSize] = {};
  reinterpret_cast<SetLocked>(natives.set_locked)(game_lock, lock.locked ? 1 : 0, context);
  return lock_properties != nullptr;
}

// Puts the server's lock on a standing container when it changed. A body has none.
void apply_lock(std::uint32_t id, State& state, const Seen& container) {
  if (container.body || (state.lock_applied && state.lock == container.lock)) {
    return;
  }
  void* entity = props::entity::by_id(state.entity);
  if (!entity) {
    return;
  }
  const bool done = put_lock(entity, container.lock, state);
  state.lock = container.lock;
  state.lock_applied = true;
  state.unlock_reported = false;
  if (log_line) {
    const char* fanciness = container.lock.fanciness == kFancinessFancy    ? "fancy"
                            : container.lock.fanciness == kFancinessCommon ? "common"
                                                                           : "the level's";
    log_line("STASH %u lock %s, difficulty %.2f, %s lock%s", id,
             container.lock.locked ? "locked" : "unlocked",
             container.lock.difficulty >= 0.0f ? container.lock.difficulty : state.own_difficulty,
             fanciness, done ? "" : " (no lock on the entity)");
  }
}

void take_away(std::uint32_t id) {
  const auto found = stashes.find(id);
  if (found == stashes.end()) {
    return;
  }
  by_entity.erase(found->second.entity);
  if (found->second.body_entity) {
    by_entity.erase(found->second.body_entity);
  }
  if (!found->second.level) {
    props::entity::remove(found->second.name);
  }
  stashes.erase(found);
}

// Whether a model path names a static mesh (.cgf, any letter case).
bool static_mesh(const std::string& model) {
  if (model.size() < 4) {
    return false;
  }
  std::string extension = model.substr(model.size() - 4);
  for (char& c : extension) {
    if (c >= 'A' && c <= 'Z') {
      c = char(c - 'A' + 'a');
    }
  }
  return extension == ".cgf";
}

// Builds a scripted container as the mesh its script named: a static mesh (.cgf) in
// slot 0, as LoadGeometry does, or a character (.cga, .cdf) in the chest's slot, as
// LoadCharacter does. The default chest when it names none or the game cannot load it.
void show_model(void* entity, const Seen& container) {
  const std::string& model = container.model;
  if (!model.empty()) {
    bool shown = false;
    if (static_mesh(model)) {
      shown = props::entity::show_mesh(entity, model);
    } else {
      shown = props::entity::load_character(entity, kChestSlot, model.c_str());
    }
    if (log_line) {
      log_line("STASH %u model %s: %s", container.id, model.c_str(),
               shown ? "shown" : "not loaded, the default chest");
    }
    if (shown) {
      return;
    }
  }
  props::entity::load_character(entity, kChestSlot, kChestModel);
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  installed = natives.services && natives.local_entity && natives.open_transfer &&
              natives.transfer_closed && natives.transfer_item && natives.inventory_by_id;
  if (installed) {
    installed = MH_CreateHook(natives.open_transfer, reinterpret_cast<void*>(open_transfer),
                              reinterpret_cast<void**>(&original_open)) == MH_OK &&
                MH_EnableHook(natives.open_transfer) == MH_OK &&
                MH_CreateHook(natives.transfer_closed, reinterpret_cast<void*>(transfer_closed),
                              reinterpret_cast<void**>(&original_closed)) == MH_OK &&
                MH_EnableHook(natives.transfer_closed) == MH_OK &&
                MH_CreateHook(natives.transfer_item, reinterpret_cast<void*>(transfer_item),
                              reinterpret_cast<void**>(&original_item)) == MH_OK &&
                MH_EnableHook(natives.transfer_item) == MH_OK;
  }
  // A lock pick of a server lock; without the hook only the key is reported.
  if (installed && natives.lockpick_unlock) {
    const bool hooked =
        MH_CreateHook(natives.lockpick_unlock, reinterpret_cast<void*>(lockpick_unlock),
                      reinterpret_cast<void**>(&original_lockpick_unlock)) == MH_OK &&
        MH_EnableHook(natives.lockpick_unlock) == MH_OK;
    if (log_line) {
      log_line("STASH lock-pick report %s", hooked ? "installed" : "unavailable");
    }
  }
  return installed;
}

void sync(const std::vector<Seen>& seen) {
  if (!installed) {
    return;
  }
  std::unordered_set<std::uint32_t> present;
  for (const Seen& container : seen) {
    present.insert(container.id);
    if (const auto found = stashes.find(container.id); found != stashes.end()) {
      follow(found->second, container);
      bind_body(container.id, found->second, container);
      apply_lock(container.id, found->second, container);
      if (found->second.chest &&
          (!found->second.empty_known || found->second.empty != container.empty)) {
        show_empty(found->second.entity, found->second, container.empty);
      }
      continue;
    }
    State state;
    std::memcpy(state.position, container.position, sizeof(state.position));
    state.name = "kcds_stash_" + std::to_string(container.id);
    if (!container.level_guid.empty()) {
      // An adopted level chest: the level's own entity, once the level has streamed it in.
      std::uint64_t key = 0;
      if (!integration::parse_level_edit_guid(container.level_guid, key)) {
        continue;
      }
      const auto found = kcds::runtime::level::entities_by_guid({key});
      const auto chest = found.find(key);
      if (chest == found.end() || !chest->second) {
        continue;
      }
      state.level = true;
      state.entity = at<std::uint32_t>(chest->second, kEntityId);
      by_entity[state.entity] = container.id;
      const auto placed = stashes.emplace(container.id, std::move(state)).first;
      apply_lock(container.id, placed->second, container);
      if (log_line) {
        log_line("STASH %u is the level chest %s", container.id, container.level_guid.c_str());
      }
      continue;
    }
    void* entity = props::entity::find(state.name);
    if (!entity) {
      entity = props::entity::spawn_class(state.name, kStashClass, container.position);
      if (entity && !container.body) {
        show_model(entity, container);
      }
    }
    if (!entity) {
      continue;
    }
    props::entity::rotate(entity, container.rotation);
    state.entity = at<std::uint32_t>(entity, kEntityId);
    by_entity[state.entity] = container.id;
    bind_body(container.id, state, container);
    if (!container.body) {
      state.chest = true;
      give_chest_prompts(entity);
      show_empty(state.entity, state, container.empty);
    }
    const auto placed = stashes.emplace(container.id, std::move(state)).first;
    apply_lock(container.id, placed->second, container);
    if (log_line) {
      log_line("STASH %u stands at %.2f %.2f %.2f", container.id, container.position[0],
               container.position[1], container.position[2]);
    }
  }
  std::vector<std::uint32_t> gone;
  for (const auto& [id, state] : stashes) {
    // One whose screen is open stays until it closes.
    if (!present.count(id) && id != open_stash) {
      gone.push_back(id);
    }
  }
  for (const std::uint32_t id : gone) {
    take_away(id);
  }
}

void aim_chests(const float* camera) {
  for (auto& [id, state] : stashes) {
    if (!state.chest) {
      continue;
    }
    // No prompt while its screen is open. While the server answers it stays as it is: a refused
    // use changes nothing on the screen.
    bool aimed = false;
    if (camera && id != open_stash) {
      const float to[3] = {state.position[0] - camera[3], state.position[1] - camera[7],
                           state.position[2] + kChestAimHeight - camera[11]};
      const float along = to[0] * camera[1] + to[1] * camera[5] + to[2] * camera[9];
      const float square = to[0] * to[0] + to[1] * to[1] + to[2] * to[2];
      aimed = along > 0.0f && square <= kChestAimReach * kChestAimReach &&
              square - along * along <= kChestAimRadius * kChestAimRadius;
    }
    if (aimed == state.aimed) {
      continue;
    }
    void* entity = props::entity::by_id(state.entity);
    void* script = entity ? script_table_of(entity) : nullptr;
    if (!script) {
      continue;
    }
    set_bool(script, "interactive", aimed);
    state.aimed = aimed;
  }
}

void set_server_bodies(std::vector<std::uint32_t> entities) {
  server_bodies.clear();
  server_bodies.insert(entities.begin(), entities.end());
}

// A virtual stash a script opened stands in as a container without a model at the local player's
// feet: the game's transfer screen needs an entity with an inventory.
std::unordered_map<std::uint32_t, State>::iterator make_stand_in(std::uint32_t id) {
  void* player = natives.local_entity ? natives.local_entity() : nullptr;
  if (!player) {
    return stashes.end();
  }
  State state;
  state.stand_in = true;
  state.name = "kcds_stash_" + std::to_string(id);
  props::entity::world_position(player, state.position);
  void* entity = props::entity::find(state.name);
  if (!entity) {
    entity = props::entity::spawn_class(state.name, kStashClass, state.position);
  }
  if (!entity) {
    return stashes.end();
  }
  state.entity = at<std::uint32_t>(entity, kEntityId);
  by_entity[state.entity] = id;
  if (log_line) {
    log_line("STASH %u stands in at the player for a script's open", id);
  }
  return stashes.emplace(id, std::move(state)).first;
}

void show(const integration::StashShow& shown) {
  if (!installed) {
    return;
  }
  auto found = stashes.find(shown.stash);
  if (found == stashes.end() && shown.open && !shown.closed) {
    found = make_stand_in(shown.stash);
  }
  if (found == stashes.end()) {
    return;
  }
  State& state = found->second;
  if (shown.closed) {
    // A use the server refused: the container's script took itself for open; the screen's own
    // close callback, as if the screen had opened and closed, sets it back.
    if (state.waiting && state.binding) {
      const std::uint32_t entity = state.used_entity ? state.used_entity : state.entity;
      std::memcpy(static_cast<std::uint8_t*>(state.binding) + kBindingEntity, &entity,
                  sizeof(entity));
      original_closed(state.binding);
    }
    state.waiting = false;
    forget_rows(state);
    // Still locked on the server: an unlock it refused is undone in this game with the next sync.
    if (state.lock.locked) {
      state.lock_applied = false;
    }
    if (log_line) {
      log_line("STASH %u: the server ended the hold", shown.stash);
    }
    return;
  }
  // Opened by a script (Stash#open), not by a use: nothing is waiting for it.
  if (shown.open && !state.waiting && !prepare_script_open(state)) {
    if (log_line) {
      log_line("STASH %u: a script's open found no inventory to show", shown.stash);
    }
    return;
  }
  if (!state.inventory && state.inventory_id) {
    state.inventory = inventory_of(state.inventory_id);
  }
  if (!state.inventory) {
    return;
  }
  state.rows = inventory::fill_container(state.inventory, shown.rows, wearer_of(state));
  if (!shown.open || !state.waiting || !state.binding) {
    return;
  }
  state.waiting = false;
  static const OwnHandler handler{own_handler_vtable()};
  granting = true;
  original_open(state.binding, const_cast<OwnHandler*>(&handler),
                int(state.used_entity ? state.used_entity : state.entity), state.inventory_id,
                state.filter.c_str(), state.header.c_str());
  granting = false;
  open_stash = shown.stash;
  if (log_line) {
    log_line("STASH %u open: %zu rows", shown.stash, shown.rows.size());
  }
}

std::vector<Report> take_reports() {
  std::lock_guard<std::mutex> lock(reports_mutex);
  return std::exchange(reports, {});
}

void reset() {
  std::vector<std::uint32_t> ids;
  for (const auto& [id, state] : stashes) {
    ids.push_back(id);
  }
  open_stash = 0;
  for (const std::uint32_t id : ids) {
    take_away(id);
  }
}

}  // namespace kcds::runtime::stash
