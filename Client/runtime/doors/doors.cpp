#include "doors.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "../props/prop_entity.hpp"

namespace kcds::runtime::doors {
namespace {

using kcd2s::integration::DoorAction;
using kcd2s::integration::DoorEntry;
using kcd2s::integration::DoorNearestReply;
using kcd2s::integration::DoorSpawn;
using kcd2s::integration::DoorState;
using kcd2s::integration::DoorUse;

// The script class of a door.
constexpr char kDoorClass[] = "AnimDoor";
// RTTI name of the door component, to be sure a lock is one before its vtable is hooked.
constexpr char kDoorComponentName[] = ".?AVC_AnimDoor@entitymodule@wh@@";

// IEntitySystem: the class registry (+ 88, FindClass + 24) and the entity iterator (slot 22: AddRef
// slot 1, Release slot 2, Next slot 4, MoveFirst slot 6); each entity's class is its slot 3 and its
// key (a 64-bit GUID) its slot 2.
constexpr std::size_t kClassRegistry = 88;
constexpr std::size_t kFindClass = 24;
constexpr std::size_t kIteratorSlot = 22;
constexpr std::size_t kAddRef = 1;
constexpr std::size_t kRelease = 2;
constexpr std::size_t kNext = 4;
constexpr std::size_t kMoveFirst = 6;
constexpr std::size_t kEntityKey = 2;
constexpr std::size_t kEntityClass = 3;
// CEntity: its id.
constexpr std::size_t kEntityId = 12;
// CEntity: the script proxy (GetProxy slot 74, type 2) and the proxy's script table (slot 18).
constexpr std::size_t kGetProxySlot = 74;
constexpr int kScriptProxy = 2;
constexpr std::size_t kScriptTableSlot = 18;
// IScriptTable: Release slot 4, GetValueAny slot 8. A ScriptAnyValue is 32 bytes: its type, then
// the value at + 8.
constexpr std::size_t kTableRelease = 4;
constexpr std::size_t kSetValueAnySlot = 7;
constexpr std::size_t kGetValueAnySlot = 8;
constexpr std::size_t kAnyValue = 8;
constexpr std::size_t kAnySize = 32;
constexpr int kAnyNull = 0;
constexpr int kAnyBool = 2;
constexpr int kAnyNumber = 4;
constexpr int kAnyString = 5;
constexpr int kAnyTable = 6;
// C_LockBase: the entity.
constexpr std::size_t kLockEntity = 56;
// The lock-by-id lookup reads the entity id at this offset of its argument.
constexpr std::size_t kLockIdOffset = 0x28;
// SetLocked reads a callback from its context at + 56; zeros are none.
constexpr std::size_t kLockContextSize = 128;

constexpr ULONGLONG kScanMilliseconds = 500;
// The doors within this far (metres) of the player are reported.
constexpr float kReportRadius = 100.0f;
// A door the player used, or is asked about, lies within this far (metres).
constexpr float kUseReach = 5.0f;
// How close an NPC this game simulates stands to a door it worked (issue 016), and how long a body
// counts as here after it was last told.
constexpr float kNpcUseReach = 3.0f;
constexpr ULONGLONG kNpcForgetMs = 2000;
constexpr float kNearestReach = 6.0f;
// A door the server set swings for about a second: until this long after it was set (ms), a
// difference is the game still moving it (or refusing it), not the player's use.
constexpr ULONGLONG kApplySettle = 2000;

using Get = void*(__fastcall*)(void*);
using Call = void(__fastcall*)(void*);
using FindClass = void*(__fastcall*)(void*, const char*);
using EntityKey = std::uint64_t(__fastcall*)(void*);
using GetProxy = void*(__fastcall*)(void*, int);
using SetValueAny = void(__fastcall*)(void* table, const char* key, const void* value, bool chain);
using GetValueAny = bool(__fastcall*)(void* table, const char* key, void* value, bool chain);
using ReleaseTable = void(__fastcall*)(void* table);
using LockOfEntityId = void*(__fastcall*)(const void* holder);
using OpenDoor = void(__fastcall*)(void* door, char force);
using CloseDoor = void(__fastcall*)(void* door, char force, char check_busy);
using SetLocked = std::int64_t(__fastcall*)(void* lock, char locked, void* context);

Natives natives;
Logger log_line = nullptr;
bool installed = false;

float local_position[3] = {};

struct Door {
  void* entity = nullptr;
  void* lock = nullptr;
  DoorEntry entry;
  bool minigame = false;  // the lock-picking minigame is running on it
};

// What the server was last told of each door (or what the player did, which it was told as a use).
std::map<std::uint64_t, DoorState> known;
// The level's own key of each door a script gave another.
std::unordered_map<std::uint64_t, std::string> own_keys;
// The lock-picking difficulty each door had before a script set one, restored when
// the script gives it back.
std::map<std::uint64_t, float> own_difficulties;
// The doors whose lock-picking minigame was seen running: the unlock that follows is a lock pick.
std::map<std::uint64_t, bool> minigame_seen;
// The side each door was last swung to, by the player or by the server.
std::map<std::uint64_t, bool> sides;
// The doors scripts hung: each one's entity name, by GUID.
std::map<std::uint64_t, std::string> hung;
// The doors set from the server's state that the game has not shown in that state yet, and when
// they were set: a door that did not get there is reported, not taken for the
// player's use. Told as a use, the other player's open or close was undone for everyone
// (one player opened a door, another still saw it closed).
std::map<std::uint64_t, ULONGLONG> applying;
ULONGLONG last_scan = 0;

template <class T>
T read(const void* base, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
  return value;
}

void** vtable_of(void* object) {
  return *static_cast<void***>(object);
}

template <class Function>
Function slot_of(void* object, std::size_t index) {
  return reinterpret_cast<Function>(vtable_of(object)[index]);
}

void say(const char* format, const char* text) {
  if (log_line) {
    log_line(format, text);
  }
}

float distance_squared(const float a[3], const float b[3]) {
  float sum = 0.0f;
  for (int axis = 0; axis < 3; ++axis) {
    sum += (a[axis] - b[axis]) * (a[axis] - b[axis]);
  }
  return sum;
}

// ---- The entity's script table ---------------------------------------------------------------

// A table of a script table (a reference the caller releases); null when there is none.
void* table_value(void* table, const char* key) {
  alignas(16) std::uint8_t value[kAnySize] = {};
  if (!table || !slot_of<GetValueAny>(table, kGetValueAnySlot)(table, key, value, false)) {
    return nullptr;
  }
  if (read<int>(value, 0) != kAnyTable) {
    return nullptr;
  }
  return read<void*>(value, kAnyValue);
}

bool bool_value(void* table, const char* key, bool fallback) {
  alignas(16) std::uint8_t value[kAnySize] = {};
  if (!table || !slot_of<GetValueAny>(table, kGetValueAnySlot)(table, key, value, false) ||
      read<int>(value, 0) != kAnyBool) {
    return fallback;
  }
  return read<std::uint8_t>(value, kAnyValue) != 0;
}

std::string text_value(void* table, const char* key) {
  alignas(16) std::uint8_t value[kAnySize] = {};
  if (!table || !slot_of<GetValueAny>(table, kGetValueAnySlot)(table, key, value, false) ||
      read<int>(value, 0) != kAnyString) {
    return {};
  }
  const char* text = read<const char*>(value, kAnyValue);
  return text ? std::string(text, strnlen(text, 256)) : std::string();
}

// A number of a script table. The script system keeps numbers as floats; the value is also
// accepted as a 32-bit or 64-bit integer when that is the one that makes sense (the door fields
// read here are -1, 0 or 1).
double number_value(void* table, const char* key, double fallback) {
  alignas(16) std::uint8_t value[kAnySize] = {};
  if (!table || !slot_of<GetValueAny>(table, kGetValueAnySlot)(table, key, value, false)) {
    return fallback;
  }
  const int type = read<int>(value, 0);
  if (type == kAnyNull || type == kAnyBool || type == kAnyString || type == kAnyTable) {
    return fallback;
  }
  const float as_float = read<float>(value, kAnyValue);
  if (as_float == 0.0f || std::fabs(as_float) == 1.0f) {
    return double(as_float);
  }
  const std::int32_t as_int = read<std::int32_t>(value, kAnyValue);
  if (as_int >= -1 && as_int <= 1) {
    return double(as_int);
  }
  const double as_double = read<double>(value, kAnyValue);
  if (as_double >= -1.0 && as_double <= 1.0) {
    return as_double;
  }
  return std::isfinite(as_float) ? double(as_float) : fallback;
}

// A fraction of a script table (the script system keeps numbers as floats), or `fallback`.
float fraction_value(void* table, const char* key, float fallback) {
  alignas(16) std::uint8_t value[kAnySize] = {};
  if (!table || !slot_of<GetValueAny>(table, kGetValueAnySlot)(table, key, value, false) ||
      read<int>(value, 0) != kAnyNumber) {
    return fallback;
  }
  const float number = read<float>(value, kAnyValue);
  return std::isfinite(number) ? number : fallback;
}

void set_text(void* table, const char* key, const char* text) {
  alignas(16) std::uint8_t value[kAnySize] = {};
  std::memcpy(value, &kAnyString, sizeof(kAnyString));
  std::memcpy(value + kAnyValue, &text, sizeof(text));
  slot_of<SetValueAny>(table, kSetValueAnySlot)(table, key, value, false);
}

void set_fraction(void* table, const char* key, float number) {
  alignas(16) std::uint8_t value[kAnySize] = {};
  std::memcpy(value, &kAnyNumber, sizeof(kAnyNumber));
  std::memcpy(value + kAnyValue, &number, sizeof(number));
  slot_of<SetValueAny>(table, kSetValueAnySlot)(table, key, value, false);
}

void* script_table_of(void* entity) {
  void* proxy = slot_of<GetProxy>(entity, kGetProxySlot)(entity, kScriptProxy);
  return proxy ? slot_of<Get>(proxy, kScriptTableSlot)(proxy) : nullptr;
}

// ---- Reading a door --------------------------------------------------------------------------

// Whether the player stands in front of the door: the door's forward axis (the second column of its
// world matrix) against the vector to the player, as AnimDoor:IsAccessedFromFront does.
bool player_in_front(void* entity) {
  constexpr std::size_t kGetWorldTM = 32;
  using GetWorldTM = const float*(__fastcall*)(void*);
  const float* matrix = slot_of<GetWorldTM>(entity, kGetWorldTM)(entity);
  const float to_door_x = matrix[3] - local_position[0];
  const float to_door_y = matrix[7] - local_position[1];
  return to_door_x * matrix[1] + to_door_y * matrix[5] >= 0.0f;
}

// Whether the key of the door fits on the side the player stands (AnimDoor:IsOnKeySide).
bool player_on_key_side(void* script, void* entity) {
  void* properties = table_value(script, "Properties");
  void* lock = table_value(properties, "Lock");
  const std::string types = text_value(lock, "esLockTypes");
  const bool reversed = bool_value(lock, "bLockReversed", false);
  if (lock) {
    slot_of<ReleaseTable>(lock, kTableRelease)(lock);
  }
  if (properties) {
    slot_of<ReleaseTable>(properties, kTableRelease)(properties);
  }
  if (types == "key/key") {
    return true;
  }
  const bool front = player_in_front(entity);
  return types == "manual/key" && ((!reversed && !front) || (reversed && front));
}

bool is_door_component(void* object) {
  if (!object) {
    return false;
  }
  void** vtable = vtable_of(object);
  const std::uint8_t* locator = static_cast<const std::uint8_t*>(vtable[-1]);
  if (!locator) {
    return false;
  }
  const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"WHGame.dll"));
  const std::uintptr_t descriptor = base + read<std::uint32_t>(locator, 12);
  const char* name = reinterpret_cast<const char*>(descriptor + 16);
  return std::strncmp(name, kDoorComponentName, sizeof(kDoorComponentName)) == 0;
}

// The door of an entity: its lock component, when that is the door's own. The game's lookup
// takes the game object of the entity, not the CEntity, so the lock is found the way
// the lock script bindings find it: by the entity id, through, which reads the id at
// + 0x28 of its argument.
void* door_lock(void* entity) {
  if (!natives.lock_of_entity_id) {
    return nullptr;
  }
  struct IdHolder {
    std::uint8_t before[kLockIdOffset];
    std::uint32_t id;
    std::uint32_t after;
  };
  IdHolder holder{};
  holder.id = read<std::uint32_t>(entity, kEntityId);
  void* lock = reinterpret_cast<LockOfEntityId>(natives.lock_of_entity_id)(&holder);
  if (!lock || read<void*>(lock, kLockEntity) != entity || !is_door_component(lock)) {
    return nullptr;
  }
  return lock;
}

bool read_door(void* entity, Door& door) {
  const std::uint64_t guid = slot_of<EntityKey>(entity, kEntityKey)(entity);
  void* lock = door_lock(entity);
  void* script = script_table_of(entity);
  if (guid == 0 || !lock || !script) {
    return false;
  }
  door.entity = entity;
  door.lock = lock;
  DoorEntry& entry = door.entry;
  entry.state.guid = guid;
  entry.state.open = number_value(script, "nDirection", -1.0) > 0.0;
  entry.state.locked = bool_value(script, "bLocked", false);
  const auto side = sides.find(guid);
  entry.state.side = side != sides.end() && side->second;
  door.minigame = number_value(script, "nUserId", 0.0) != 0.0;
  void* properties = table_value(script, "Properties");
  void* lock_properties = table_value(properties, "Lock");
  // AnimDoor.lua keeps whether its lock can be picked in `lockpickable`, from the level's
  // Properties.Lock.bCanLockPick (read there too before the script set it up).
  entry.canBePicked =
      bool_value(script, "lockpickable", bool_value(lock_properties, "bCanLockPick", false));
  entry.key = text_value(lock_properties, "guidItemClassId");
  // The level's own key, not one a script put here.
  if (const auto own = own_keys.find(guid); own != own_keys.end()) {
    entry.key = own->second;
  }
  entry.model = text_value(properties, "object_Model");
  if (entry.model.size() > kcd2s::integration::kMaxDoorModelLength) {
    entry.model.clear();
  }
  entry.builtLocked = bool_value(lock_properties, "bLocked", false);
  // The level's (or the game's) difficulty, as before a script set one.
  const auto own = own_difficulties.find(guid);
  entry.difficulty = own != own_difficulties.end()
                         ? own->second
                         : fraction_value(lock_properties, "fLockDifficulty", 0.5f);
  if (lock_properties) {
    slot_of<ReleaseTable>(lock_properties, kTableRelease)(lock_properties);
  }
  if (properties) {
    slot_of<ReleaseTable>(properties, kTableRelease)(properties);
  }
  entry.name = props::entity::name_of(entity, kcd2s::integration::kMaxDoorNameLength);
  props::entity::world_position(entity, entry.position);
  if (entry.key.size() > kcd2s::integration::kMaxDoorKeyLength) {
    entry.key.clear();
  }
  return true;
}

// The doors within `radius` metres of the player: the entities of class AnimDoor, as the game's
// System.GetEntitiesByClass binding walks them.
std::vector<Door> doors_near(float radius) {
  std::vector<Door> found;
  void* system = natives.entity_system ? natives.entity_system() : nullptr;
  if (!system) {
    return found;
  }
  void* registry = read<Get>(*static_cast<void**>(system), kClassRegistry)(system);
  void* door_class =
      registry ? read<FindClass>(*static_cast<void**>(registry), kFindClass)(registry, kDoorClass)
               : nullptr;
  void* iterator = door_class ? slot_of<Get>(system, kIteratorSlot)(system) : nullptr;
  if (!iterator) {
    return found;
  }
  slot_of<Call>(iterator, kAddRef)(iterator);
  slot_of<Call>(iterator, kMoveFirst)(iterator);
  while (void* entity = slot_of<Get>(iterator, kNext)(iterator)) {
    if (slot_of<Get>(entity, kEntityClass)(entity) != door_class) {
      continue;
    }
    float position[3] = {};
    props::entity::world_position(entity, position);
    if (distance_squared(position, local_position) > radius * radius) {
      continue;
    }
    Door door;
    if (read_door(entity, door)) {
      found.push_back(std::move(door));
    }
  }
  slot_of<Call>(iterator, kRelease)(iterator);
  return found;
}

// ---- The player's own use --------------------------------------------------------------------

const char* action_name(DoorAction action) {
  switch (action) {
    case DoorAction::Open:
      return "open";
    case DoorAction::Close:
      return "close";
    case DoorAction::Unlock:
      return "unlock";
    case DoorAction::Lockpick:
      return "lockpick";
  }
  return "?";
}

void send_use(std::uint64_t guid, DoorAction action, bool key_side, std::uint32_t npc = 0) {
  DoorUse use;
  use.guid = guid;
  use.action = action;
  use.keySide = key_side;
  use.npc = npc;
  if (natives.send_use) {
    natives.send_use(use);
  }
  if (log_line) {
    log_line("DOORS use %s %s npc=%u", kcd2s::integration::door_guid_text(guid).c_str(),
             action_name(action), npc);
  }
}

// The NPCs this game simulates and where they stood when last told (issue 016).
struct SimulatedNpc {
  float position[3] = {};
  ULONGLONG told = 0;
};
std::unordered_map<std::uint32_t, SimulatedNpc> simulated_npcs;

// The NPC this game simulates that stands at the door, nearer than `within_squared` when given,
// 0 for none.
std::uint32_t npc_at(const float door[3], float within_squared = kNpcUseReach * kNpcUseReach) {
  const ULONGLONG now = GetTickCount64();
  std::uint32_t nearest = 0;
  float nearest_squared = (std::min)(within_squared, kNpcUseReach * kNpcUseReach);
  for (auto it = simulated_npcs.begin(); it != simulated_npcs.end();) {
    if (now - it->second.told > kNpcForgetMs) {
      it = simulated_npcs.erase(it);
      continue;
    }
    const float squared = distance_squared(door, it->second.position);
    if (squared <= nearest_squared) {
      nearest = it->first;
      nearest_squared = squared;
    }
    ++it;
  }
  return nearest;
}

// ---- The server's state ----------------------------------------------------------------------

// The state is known before the read sees it, so the read does not take it for the player's.
// The lock-picking difficulty a script set on the door (the minigame reads it from the door's
// Properties.Lock at its start), or the door's own again.
void put_difficulty(const Door& door, float difficulty) {
  const std::uint64_t guid = door.entry.state.guid;
  const auto own = own_difficulties.find(guid);
  if (difficulty < 0.0f && own == own_difficulties.end()) {
    return;
  }
  void* script = script_table_of(door.entity);
  void* properties = table_value(script, "Properties");
  void* lock = table_value(properties, "Lock");
  if (lock) {
    if (difficulty >= 0.0f) {
      if (own == own_difficulties.end()) {
        own_difficulties[guid] = door.entry.difficulty;
      }
      set_fraction(lock, "fLockDifficulty", difficulty);
    } else {
      set_fraction(lock, "fLockDifficulty", own->second);
      own_difficulties.erase(own);
    }
    if (log_line) {
      log_line("DOORS door %s lock difficulty %.2f",
               kcd2s::integration::door_guid_text(guid).c_str(),
               fraction_value(lock, "fLockDifficulty", -1.0f));
    }
    slot_of<ReleaseTable>(lock, kTableRelease)(lock);
  }
  if (properties) {
    slot_of<ReleaseTable>(properties, kTableRelease)(properties);
  }
}

// The key a script set on the door, or the level's own again: the game's own check
// reads Properties.Lock.guidItemClassId, so it offers Unlock to whoever carries it.
void put_key(const Door& door, const std::string& key) {
  const std::uint64_t guid = door.entry.state.guid;
  const auto own = own_keys.find(guid);
  if (key.empty() && own == own_keys.end()) {
    return;
  }
  void* script = script_table_of(door.entity);
  void* properties = table_value(script, "Properties");
  void* lock = table_value(properties, "Lock");
  if (lock) {
    if (!key.empty()) {
      if (own == own_keys.end()) {
        own_keys[guid] = door.entry.key;
      }
      set_text(lock, "guidItemClassId", key.c_str());
    } else {
      set_text(lock, "guidItemClassId", own->second.c_str());
      own_keys.erase(own);
    }
    slot_of<ReleaseTable>(lock, kTableRelease)(lock);
  }
  if (properties) {
    slot_of<ReleaseTable>(properties, kTableRelease)(properties);
  }
}

void apply_state(const Door& door, const DoorState& want) {
  alignas(16) std::uint8_t context[kLockContextSize] = {};
  const auto set_locked = reinterpret_cast<SetLocked>(natives.set_locked);
  const DoorState& now = door.entry.state;
  // Unlocked before it opens; locked after it closed (locking an open door closes it).
  if (now.locked && !want.locked) {
    set_locked(door.lock, 0, context);
  }
  if (!now.open && want.open) {
    reinterpret_cast<OpenDoor>(natives.open_door)(door.lock, 1);
  } else if (now.open && !want.open) {
    reinterpret_cast<CloseDoor>(natives.close_door)(door.lock, 1, 0);
  }
  if (!now.locked && want.locked) {
    set_locked(door.lock, 1, context);
  }
  put_difficulty(door, want.difficulty);
  put_key(door, want.key);
  sides[want.guid] = want.side;
  known[want.guid] = want;
  if (!(now == want)) {
    applying[want.guid] = GetTickCount64();
    if (log_line) {
      log_line("DOORS set %s open=%d locked=%d (was open=%d locked=%d)",
               kcd2s::integration::door_guid_text(want.guid).c_str(), want.open ? 1 : 0,
               want.locked ? 1 : 0, now.open ? 1 : 0, now.locked ? 1 : 0);
    }
  }
  if (want.open && now.side != want.side) {
    say("DOORS the side of door %s is the game's own (the local player decides it)",
        kcd2s::integration::door_guid_text(want.guid).c_str());
  }
}

void apply_sets(const std::vector<DoorState>& sets) {
  if (sets.empty()) {
    return;
  }
  std::vector<Door> doors = doors_near(1.0e9f);
  for (const DoorState& want : sets) {
    bool found = false;
    for (const Door& door : doors) {
      if (door.entry.state.guid == want.guid) {
        apply_state(door, want);
        found = true;
        break;
      }
    }
    if (!found) {
      say("DOORS door %s is not in this game's level",
          kcd2s::integration::door_guid_text(want.guid).c_str());
    }
  }
}

// ---- Reporting -------------------------------------------------------------------------------

// A lock the player opened (a key or a lock pick) since the last read, told as a use.
void note_lock_change(const Door& door, const DoorState& before) {
  const DoorState& now = door.entry.state;
  const float player_squared = distance_squared(door.entry.position, local_position);
  if (!before.locked || now.locked || player_squared > kUseReach * kUseReach) {
    return;
  }
  // An NPC nearer the door unlocked it with its own key, not the player.
  if (npc_at(door.entry.position, player_squared) != 0) {
    return;
  }
  const std::uint64_t guid = now.guid;
  const bool picked = minigame_seen[guid];
  minigame_seen[guid] = false;
  void* script = script_table_of(door.entity);
  send_use(guid, picked ? DoorAction::Lockpick : DoorAction::Unlock,
           script && player_on_key_side(script, door.entity));
}

// A door within the player's reach that opened or closed since the last read, and not by this
// module (the server's states are known before they are applied): the player worked it, which
// the server is told as a use. Reported as a plain change instead, the server would put it back.
bool note_open_change(const Door& door, const DoorState& before) {
  const DoorState& now = door.entry.state;
  if (before.open == now.open) {
    return false;
  }
  const float player_squared = distance_squared(door.entry.position, local_position);
  if (player_squared > kUseReach * kUseReach) {
    // Not the player's: an NPC this game simulates worked it (issue 016), or nobody did.
    const std::uint32_t npc = npc_at(door.entry.position);
    if (npc == 0) {
      return false;
    }
    send_use(now.guid, now.open ? DoorAction::Open : DoorAction::Close, false, npc);
    return true;
  }
  // An NPC nearer the door than the player worked it: told as the player's use, the server
  // could refuse it and shut the door in front of the NPC.
  if (const std::uint32_t npc = npc_at(door.entry.position, player_squared)) {
    send_use(now.guid, now.open ? DoorAction::Open : DoorAction::Close, false, npc);
    return true;
  }
  void* script = script_table_of(door.entity);
  const bool back = !player_in_front(door.entity);
  send_use(now.guid, now.open ? DoorAction::Open : DoorAction::Close,
           script && player_on_key_side(script, door.entity));
  if (now.open) {
    sides[now.guid] = back;
  }
  return true;
}

void scan_doors() {
  std::vector<Door> doors = doors_near(kReportRadius);
  std::vector<DoorEntry> changed;
  const ULONGLONG tick = GetTickCount64();
  for (const Door& door : doors) {
    const DoorState& now = door.entry.state;
    if (door.minigame) {
      minigame_seen[now.guid] = true;
    }
    const auto before = known.find(now.guid);
    const auto set = applying.find(now.guid);
    if (set != applying.end() && before != known.end()) {
      if (before->second.open == now.open && before->second.locked == now.locked) {
        // The game shows the server's state now.
        applying.erase(set);
        continue;
      }
      if (tick - set->second < kApplySettle) {
        // Still swinging to the server's state.
        continue;
      }
      // The game did not get there (it refused, or the door was busy): the server hears how it
      // is and sends its state again.
      applying.erase(set);
      if (log_line) {
        log_line("DOORS door %s did not take the server's state (open=%d locked=%d here)",
                 kcd2s::integration::door_guid_text(now.guid).c_str(), now.open ? 1 : 0,
                 now.locked ? 1 : 0);
      }
      changed.push_back(door.entry);
      continue;
    }
    if (before == known.end()) {
      changed.push_back(door.entry);
    } else if (!(before->second == now)) {
      if (before->second.locked && !now.locked) {
        // The player unlocked it: the server is told as a use, which carries the new state.
        note_lock_change(door, before->second);
        // "Unlock and open" does both in one go: the open is the player's use too, or the door
        // stays shut in every other game.
        if (before->second.open != now.open && note_open_change(door, before->second)) {
          before->second.side = sides[now.guid];
        }
        const bool side = before->second.side;
        before->second = now;
        before->second.side = side;
      } else if (note_open_change(door, before->second)) {
        before->second = now;
        before->second.side = sides[now.guid];
      } else {
        changed.push_back(door.entry);
      }
    }
  }
  // A door counts as reported once its message was queued; while the world is not ready nothing
  // is sent and the next read tries again.
  const std::size_t per_message = kcd2s::integration::kMaxDoorsPerReport;
  for (std::size_t first = 0; first < changed.size(); first += per_message) {
    const std::size_t last = (std::min)(changed.size(), first + per_message);
    const std::vector<DoorEntry> batch(changed.begin() + first, changed.begin() + last);
    if (!natives.send_report || !natives.send_report(batch)) {
      return;
    }
    for (const DoorEntry& entry : batch) {
      known[entry.state.guid] = entry.state;
    }
  }
}

// ---- Doors scripts hang ---------------------------------------------------------

std::string hung_name(std::uint64_t guid) {
  return "kcds_door_" + kcd2s::integration::door_guid_text(guid);
}

void take_down(std::uint64_t guid) {
  const auto found = hung.find(guid);
  if (found == hung.end()) {
    return;
  }
  props::entity::remove(found->second);
  hung.erase(found);
  known.erase(guid);
  sides.erase(guid);
  own_difficulties.erase(guid);
  minigame_seen.erase(guid);
}

void hang(const DoorSpawn& door) {
  using props::entity::ScriptProperty;
  take_down(door.guid);
  if (door.remove) {
    say("DOORS hung door %s taken down", kcd2s::integration::door_guid_text(door.guid).c_str());
    return;
  }
  // As the level's doors have them in their Properties (Data/Levels/*/level.pak, AnimDoor).
  std::vector<ScriptProperty> properties = {
      {"object_Model", door.model.c_str()},
      {"esDoorAnimSet", door.anim_set.c_str()},
      {"soclass_SmartObjectHelpers", door.helpers.c_str()},
      {"guidSmartObjectType", door.smart_object_type.c_str()},
      {"bRigidBody", nullptr, door.rigid ? 1.0f : 0.0f, "Physics", true},
      {"bPushableByPlayers", nullptr, door.pushable ? 1.0f : 0.0f, "Physics", true},
      {"bLocked", nullptr, door.locked ? 1.0f : 0.0f, "Lock", true},
      {"esLockTypes", door.lock_types.c_str(), 0.0f, "Lock"},
      {"bCanLockPick", nullptr, door.canBePicked ? 1.0f : 0.0f, "Lock", true},
      {"guidItemClassId", door.key.c_str(), 0.0f, "Lock"},
  };
  if (door.difficulty >= 0.0f) {
    properties.push_back({"fLockDifficulty", nullptr, door.difficulty, "Lock"});
    properties.push_back({"bLockDifficultyOverride", nullptr, 1.0f, "Lock", true});
  }
  const std::string name = hung_name(door.guid);
  void* entity = props::entity::spawn_class_with(name, kDoorClass, door.position, door.rotation,
                                                 properties, door.guid);
  if (entity) {
    hung[door.guid] = name;
  }
  if (log_line) {
    log_line("DOORS hung door %s %s: %s", kcd2s::integration::door_guid_text(door.guid).c_str(),
             door.model.c_str(), entity ? "hung" : "not hung");
  }
}

// ---- The nearest door ------------------------------------------------------------------------

void answer_nearest(std::uint32_t request) {
  DoorNearestReply reply;
  reply.request = request;
  const std::vector<Door> doors = doors_near(kNearestReach);
  const Door* nearest = nullptr;
  float nearest_distance = 0.0f;
  for (const Door& door : doors) {
    const float distance = distance_squared(door.entry.position, local_position);
    if (!nearest || distance < nearest_distance) {
      nearest = &door;
      nearest_distance = distance;
    }
  }
  if (nearest) {
    reply.found = true;
    reply.door = nearest->entry;
  } else {
    reply.reason = "no door within reach";
  }
  if (natives.send_reply) {
    natives.send_reply(reply);
  }
}

}  // namespace

bool install(const Natives& found, Logger logger) {
  natives = found;
  log_line = logger;
  installed = natives.open_door && natives.close_door && natives.set_locked &&
              natives.lock_of_entity_id && natives.entity_system;
  return installed;
}

void frame(void* local_entity, const std::vector<DoorState>& sets,
           const std::vector<std::uint32_t>& nearest_requests,
           const std::vector<DoorSpawn>& spawns) {
  if (!installed || !local_entity) {
    return;
  }
  props::entity::world_position(local_entity, local_position);
  for (const DoorSpawn& door : spawns) {
    hang(door);
  }
  apply_sets(sets);
  for (const std::uint32_t request : nearest_requests) {
    answer_nearest(request);
  }
  const ULONGLONG now = GetTickCount64();
  if (now - last_scan >= kScanMilliseconds) {
    last_scan = now;
    scan_doors();
  }
}

PreviewState preview_state(void* entity) {
  PreviewState state;
  Door door;
  if (!installed || !entity || !read_door(entity, door)) {
    return state;
  }
  state.door = true;
  state.open = door.entry.state.open;
  state.locked = door.entry.state.locked;
  return state;
}

bool preview(void* entity, bool open) {
  Door door;
  if (!installed || !entity || !read_door(entity, door)) {
    return false;
  }
  if (open && door.entry.state.locked) {
    alignas(16) std::uint8_t context[kLockContextSize] = {};
    reinterpret_cast<SetLocked>(natives.set_locked)(door.lock, 0, context);
  }
  if (open) {
    reinterpret_cast<OpenDoor>(natives.open_door)(door.lock, 1);
  } else {
    reinterpret_cast<CloseDoor>(natives.close_door)(door.lock, 1, 0);
  }
  if (log_line) {
    log_line("DOORS preview %s %s",
             kcd2s::integration::door_guid_text(door.entry.state.guid).c_str(),
             open ? "open" : "close");
  }
  return true;
}

void note_simulated_npc(std::uint32_t npc, float x, float y, float z) {
  SimulatedNpc& body = simulated_npcs[npc];
  body.position[0] = x;
  body.position[1] = y;
  body.position[2] = z;
  body.told = GetTickCount64();
}

void reset() {
  simulated_npcs.clear();
  while (!hung.empty()) {
    take_down(hung.begin()->first);
  }
  known.clear();
  minigame_seen.clear();
  sides.clear();
  applying.clear();
  last_scan = 0;
}

}  // namespace kcds::runtime::doors
