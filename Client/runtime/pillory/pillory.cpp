#include "pillory.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "MinHook.h"

#include "../props/prop_entity.hpp"

namespace kcds::runtime::pillory {
namespace {

using HandlerGetter = void*(__fastcall*)();
using UnstanceInit = void*(__fastcall*)(void* unstance);
using UnstanceByName = bool(__fastcall*)(std::uintptr_t unused, std::uint64_t location,
                                         const char* name, void* unstance);
using UnstanceApply = char(__fastcall*)(void* handler, void* unstance, char instant, char joined);
using SetLocked = std::int64_t(__fastcall*)(void* lock, char locked, void* context);
using ReportUnlocked = std::int64_t(__fastcall*)(void* bind, void* call);
using ScriptLock = void*(__fastcall*)(void* bind, void* call);

// An S_Unstance is 56 bytes; its initializer sets id -1 and an empty text.
constexpr std::size_t kUnstanceSize = 64;
// C_LockBase: the lock's entity (the CEntity: live, its id and name were the Lua entity's;
// of it, which SetLocked uses for the Lock/Unlock state, is another object).
constexpr std::size_t kLockEntity = 56;
// SetLocked's third argument is read at + 56 for a callback; zero is none.
constexpr std::size_t kLockContextSize = 128;

// The lock of a pillory lies within this far (metres) of its holder entity.
constexpr float kLockReach = 4.0f;
// A put-in or take-out the game keeps refusing is given up after this long.
constexpr ULONGLONG kGiveUpMilliseconds = 15000;
constexpr ULONGLONG kLogMilliseconds = 2000;
// The name of a pillory's lock entity starts with this ("pillory_lockpickable[Pillory/
// pillory1:pillory_prefab[Pillory/pillory1]_<GUID>]").
constexpr char kLockNamePrefix[] = "pillory_lockpickable";
// A pillory's holder: an entity of this class whose name starts with the prefix (the
// village pillory's "so_pillory[Pillory/pillory1:pillory_prefab[...]_<GUID>]", the only one in
// Trosecko's level).
constexpr char kHolderClass[] = "SmartObjectHolder";
constexpr char kHolderNamePrefix[] = "so_pillory";
// Locks opened between two frames that wait to be reported (a burst keeps the first ones).
constexpr std::size_t kMaxOpenedLocks = 8;

const char* const kUnstanceName = "Pillory";

Natives natives;
Logger log_line = nullptr;
bool installed = false;
SetLocked original_set_locked = nullptr;
ReportUnlocked original_report_unlocked = nullptr;

// The holder the server wants the player in ("" none), the one the player is in, and when the
// current want began.
std::string wanted;
std::string inside;
ULONGLONG wanted_since = 0;
ULONGLONG last_log = 0;
// The player's stay ended by a lock pick: the take-out is retried and no second report follows.
bool lockpick_reported = false;
// The server asked for the nearest pillory: once the player is in, the client says which it was.
bool entered_unreported = false;
ULONGLONG leaving_since = 0;

// Set by the SetLocked hook, read by the frame.
std::atomic<bool> in_pillory{false};
std::atomic<bool> lock_opened{false};
// What the hook needs of the stay: the holder and its position; and the last pillory lock seen
// opened, for the next put-in to lock it again. The hook may run on another thread.
std::mutex lock_mutex;
std::string hook_holder;
float holder_position[3] = {};
void* remembered_lock = nullptr;
std::string remembered_holder;
// The names of the pillory locks opened in this game since the last frame (under lock_mutex).
std::vector<std::string> opened_locks;

template <class T>
T read(const void* base, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
  return value;
}

void say(const char* format, const char* text) {
  if (log_line) {
    log_line(format, text);
  }
}

// A log line at most every kLogMilliseconds, so a retried call does not flood the log.
bool may_log(ULONGLONG now) {
  if (now - last_log < kLogMilliseconds) {
    return false;
  }
  last_log = now;
  return true;
}

using Reports = std::vector<kcd2s::integration::PilloryReport>;

// A report of the player's own stay (kPilloryLockpicked or kPilloryLeft).
void report_stay(Reports& reports, std::uint8_t reason) {
  kcd2s::integration::PilloryReport report;
  report.reason = reason;
  reports.push_back(std::move(report));
}

// The pillory the player went into after a put-in into the nearest one (kPilloryEntered).
void report_entered(Reports& reports, const std::string& holder) {
  kcd2s::integration::PilloryReport report;
  report.reason = kcd2s::integration::kPilloryEntered;
  report.lock = holder;
  reports.push_back(std::move(report));
}

// The name of the pillory holder nearest to the local player, or "" when the level has none.
// Walks the entity system as the game's System.GetEntitiesByClass binding does:
// the class registry (system + 88, FindClass + 24), the entity iterator (system slot 22: AddRef
// slot 1, Release slot 2, Next slot 4, MoveFirst slot 6) and each entity's class (slot 3).
std::string nearest_holder(void* local_entity) {
  constexpr std::size_t kClassRegistry = 88;
  constexpr std::size_t kFindClass = 24;
  constexpr std::size_t kIteratorSlot = 22;
  constexpr std::size_t kAddRef = 1;
  constexpr std::size_t kRelease = 2;
  constexpr std::size_t kNext = 4;
  constexpr std::size_t kMoveFirst = 6;
  constexpr std::size_t kEntityClass = 3;
  using Get = void*(__fastcall*)(void*);
  using FindClass = void*(__fastcall*)(void*, const char*);
  using Call = void(__fastcall*)(void*);
  void* system = natives.entity_system ? natives.entity_system() : nullptr;
  if (!system || !local_entity) {
    return {};
  }
  auto slot_of = [](void* object, std::size_t index) {
    return (*static_cast<void***>(object))[index];
  };
  void* registry =
      reinterpret_cast<Get>(read<void*>(*static_cast<void**>(system), kClassRegistry))(system);
  void* holder_class =
      registry ? reinterpret_cast<FindClass>(read<void*>(*static_cast<void**>(registry),
                                                         kFindClass))(registry, kHolderClass)
               : nullptr;
  void* iterator =
      holder_class ? reinterpret_cast<Get>(slot_of(system, kIteratorSlot))(system) : nullptr;
  if (!iterator) {
    return {};
  }
  float here[3] = {};
  props::entity::world_position(local_entity, here);
  std::string nearest;
  float nearest_distance = 0.0f;
  reinterpret_cast<Call>(slot_of(iterator, kAddRef))(iterator);
  reinterpret_cast<Call>(slot_of(iterator, kMoveFirst))(iterator);
  while (void* entity = reinterpret_cast<Get>(slot_of(iterator, kNext))(iterator)) {
    if (reinterpret_cast<Get>(slot_of(entity, kEntityClass))(entity) != holder_class) {
      continue;
    }
    const std::string name =
        props::entity::name_of(entity, kcd2s::integration::kPilloryNameMax + 1);
    if (name.rfind(kHolderNamePrefix, 0) != 0 || !kcd2s::integration::pillory_name_valid(name)) {
      continue;
    }
    float there[3] = {};
    props::entity::world_position(entity, there);
    float distance = 0.0f;
    for (int axis = 0; axis < 3; ++axis) {
      distance += (there[axis] - here[axis]) * (there[axis] - here[axis]);
    }
    if (nearest.empty() || distance < nearest_distance) {
      nearest = name;
      nearest_distance = distance;
    }
  }
  reinterpret_cast<Call>(slot_of(iterator, kRelease))(iterator);
  return nearest;
}

// The pillory locks opened in this game since the last frame, as reports to the server.
void report_opened_locks(Reports& reports) {
  std::vector<std::string> names;
  {
    std::lock_guard<std::mutex> guard(lock_mutex);
    names.swap(opened_locks);
  }
  for (std::string& name : names) {
    say("PILLORY the pillory lock '%s' was opened in this game", name.c_str());
    kcd2s::integration::PilloryReport report;
    report.reason = kcd2s::integration::kPilloryLockOpened;
    report.lock = std::move(name);
    reports.push_back(std::move(report));
  }
}

void* handler_object() {
  return reinterpret_cast<HandlerGetter>(natives.handler)();
}

// The entity's WUID, the 64-bit handle the game's unstance code takes, as the game's script
// binding GetMyWUID does it: the entity's key (its vtable + 16) looked up in the
// WUID map (on the global). Its top byte is the kind (the player
// 5, 0x0500000000000251; the village pillory's holder 8, 0x08000000000019CB).
bool handle_of(void* entity, std::uint64_t& handle) {
  constexpr std::size_t kEntityKeySlot = 2;
  void* map = natives.wuid_map ? *static_cast<void**>(natives.wuid_map) : nullptr;
  if (!entity || !map) {
    return false;
  }
  using EntityKey = std::uint64_t(__fastcall*)(void*);
  using WuidLookup = const std::uint64_t*(__fastcall*)(void*, std::uint64_t);
  const std::uint64_t key = (*static_cast<EntityKey**>(entity))[kEntityKeySlot](entity);
  const std::uint64_t* found = reinterpret_cast<WuidLookup>(natives.wuid_lookup)(map, key);
  handle = found ? *found : 0;
  return handle != 0 && handle != ~std::uint64_t(0);
}

// Starts the unstance "Pillory" at the holder with that handle, as the game's script binding does
// for ChangeUnstance.
bool put_in(std::uint64_t holder_handle) {
  alignas(16) std::uint8_t unstance[kUnstanceSize] = {};
  reinterpret_cast<UnstanceInit>(natives.unstance_init)(unstance);
  const bool known = reinterpret_cast<UnstanceByName>(natives.unstance_by_name)(
      0, holder_handle, kUnstanceName, unstance);
  if (!known) {
    return false;
  }
  return reinterpret_cast<UnstanceApply>(natives.unstance_apply)(handler_object(), unstance, 0,
                                                                 1) != 0;
}

// Ends the unstance: an empty S_Unstance, as the game's own leave applies.
bool take_out() {
  alignas(16) std::uint8_t unstance[kUnstanceSize] = {};
  reinterpret_cast<UnstanceInit>(natives.unstance_init)(unstance);
  return reinterpret_cast<UnstanceApply>(natives.unstance_apply)(handler_object(), unstance, 0,
                                                                 0) != 0;
}

// Locks the pillory's lock again when an earlier stay ended with it opened: the lock stays open
// until the game locks it, and a pillory with an open lock cannot be picked.
void lock_again(const std::string& holder) {
  void* lock = nullptr;
  {
    std::lock_guard<std::mutex> guard(lock_mutex);
    if (remembered_holder == holder) {
      lock = remembered_lock;
    }
  }
  if (!lock || !original_set_locked) {
    return;
  }
  alignas(16) std::uint8_t context[kLockContextSize] = {};
  original_set_locked(lock, 1, context);
}

// A pillory lock opened in this game is told to the server, whoever is in a pillory: the lock state
// is not synced between games, so a lock another player picks is opened in that player's game only.
void note_opened_lock(void* entity) {
  const std::string name = props::entity::name_of(entity, kcd2s::integration::kPilloryNameMax + 1);
  if (name.compare(0, sizeof(kLockNamePrefix) - 1, kLockNamePrefix) != 0 ||
      !kcd2s::integration::pillory_name_valid(name)) {
    return;
  }
  std::lock_guard<std::mutex> guard(lock_mutex);
  if (opened_locks.size() < kMaxOpenedLocks &&
      std::find(opened_locks.begin(), opened_locks.end(), name) == opened_locks.end()) {
    opened_locks.push_back(name);
  }
}

// An unlock of a lock: told to the server when it is a pillory's, and, when it lies at the pillory
// the player is in, the end of the player's stay (another player's pick or a key).
void note_unlock(void* lock) {
  void* entity = read<void*>(lock, kLockEntity);
  if (!entity) {
    return;
  }
  note_opened_lock(entity);
  if (!in_pillory.load()) {
    return;
  }
  float at[3] = {};
  props::entity::world_position(entity, at);
  std::lock_guard<std::mutex> guard(lock_mutex);
  const float dx = at[0] - holder_position[0];
  const float dy = at[1] - holder_position[1];
  const float dz = at[2] - holder_position[2];
  if (dx * dx + dy * dy + dz * dz > kLockReach * kLockReach) {
    return;
  }
  remembered_lock = lock;
  remembered_holder = hook_holder;
  lock_opened.store(true);
}

std::int64_t __fastcall hooked_set_locked(void* lock, char locked, void* context) {
  const std::int64_t result = original_set_locked(lock, locked, context);
  if (!locked && lock) {
    note_unlock(lock);
  }
  return result;
}

// The lock script's ReportUnlocked (C_ScriptBindLockBase slot 6), the game's other way
// a lock opens: Lockpickable:Unlock (a key, from Lockpickable:OnUsed) only tells the lock's unlock
// listeners (lock + 80), without SetLocked. The game's own pillory module lets its prisoner out on
// an unlock as on a pick, so both end the stay. The lock is the one the script call names
// (the lookup ReportUnlocked itself makes).
std::int64_t __fastcall hooked_report_unlocked(void* bind, void* call) {
  void* lock = nullptr;
  if (bind && call) {
    lock = reinterpret_cast<ScriptLock>(natives.script_lock)(bind, call);
  }
  const std::int64_t result = original_report_unlocked(bind, call);
  if (lock) {
    note_unlock(lock);
  }
  return result;
}

// The game's action map "unstance" holds one action, unstance_end (Esc, "Stop"), with which a
// player leaves an unstance by themselves. While the server keeps the player in a pillory the map
// is off; the game turns it on when the unstance starts, so it is kept off every frame. Through
// the action map manager (services + 8, the framework's vtable + 224; EnableActionMap + 232).
void enable_unstance_map(bool enable) {
  constexpr std::size_t kServicesFramework = 8;
  constexpr std::size_t kActionMapManagerSlot = 28;
  constexpr std::size_t kEnableActionMapSlot = 29;
  using Services = void*(__fastcall*)();
  using Getter = void*(__fastcall*)(void*);
  using EnableActionMap = void(__fastcall*)(void*, const char*, bool);
  void* services = natives.services ? reinterpret_cast<Services>(natives.services)() : nullptr;
  void* framework = services ? read<void*>(services, kServicesFramework) : nullptr;
  void* manager =
      framework ? (*static_cast<Getter**>(framework))[kActionMapManagerSlot](framework) : nullptr;
  if (manager) {
    (*static_cast<EnableActionMap**>(manager))[kEnableActionMapSlot](manager, "unstance", enable);
  }
}

// The stay is over. The game's own leave ends the unstance together with its action map, so a
// player who is out keeps the map off: turned on outside an unstance, it shows "Stop" (Esc) and
// holds the player in place (a lock pick). Only a player the game would not take out gets
// Esc back, to leave the stocks by themselves.
void forget_stay(bool still_inside) {
  if (!inside.empty() && still_inside) {
    enable_unstance_map(true);
  }
  inside.clear();
  in_pillory.store(false);
  lock_opened.store(false);
  std::lock_guard<std::mutex> guard(lock_mutex);
  hook_holder.clear();
}

// The server wants the player in a pillory: finds the holder in the level, puts the player's
// weapon away and goes in.
void try_put_in(ULONGLONG now, void* local_actor, Reports& reports) {
  void* holder = props::entity::find(wanted);
  std::uint64_t handle = 0;
  bool done = false;
  if (!holder) {
    if (may_log(now) && log_line) {
      log_line("PILLORY holder entity '%s' is not in the level (yet)", wanted.c_str());
    }
  } else if (!handle_of(holder, handle)) {
    // The holder has no WUID: no put-in can work, nothing to wait for.
    if (log_line) {
      log_line("PILLORY holder '%s' has no WUID (%llx)", wanted.c_str(),
               static_cast<unsigned long long>(handle));
    }
    wanted.clear();
    report_stay(reports, kcd2s::integration::kPilloryLeft);
    return;
  } else {
    lock_again(wanted);
    if (natives.holster && natives.holster(local_actor)) {
      say("PILLORY the player's weapon is put away for '%s'", wanted.c_str());
    }
    done = put_in(handle);
    if (!done && may_log(now) && log_line) {
      log_line("PILLORY the game refused to put the player into '%s' (handle %llx)", wanted.c_str(),
               static_cast<unsigned long long>(handle));
    }
  }
  if (done) {
    inside = wanted;
    {
      std::lock_guard<std::mutex> guard(lock_mutex);
      props::entity::world_position(holder, holder_position);
      hook_holder = inside;
    }
    lockpick_reported = false;
    in_pillory.store(true);
    say("PILLORY the player is in '%s'", inside.c_str());
    return;
  }
  if (now - wanted_since > kGiveUpMilliseconds) {
    say("PILLORY gave up putting the player into '%s'", wanted.c_str());
    wanted.clear();
    report_stay(reports, kcd2s::integration::kPilloryLeft);
  }
}

// The player is in a pillory the server no longer wants (or another one): out of it.
void try_take_out(ULONGLONG now, Reports& reports) {
  if (leaving_since == 0) {
    leaving_since = now;
  }
  const bool out = take_out();
  if (!out && now - leaving_since <= kGiveUpMilliseconds) {
    return;
  }
  say(out ? "PILLORY the player is out of '%s'" : "PILLORY gave up taking the player out of '%s'",
      inside.c_str());
  forget_stay(!out);
  leaving_since = 0;
  // The server hears about a release it ordered; a lock pick was reported already, and a move to
  // another pillory is not a release.
  if (wanted.empty() && !lockpick_reported) {
    report_stay(reports, kcd2s::integration::kPilloryLeft);
  }
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  installed = false;
  if (!natives.handler || !natives.unstance_init || !natives.unstance_by_name ||
      !natives.unstance_apply || !natives.set_locked || !natives.wuid_lookup || !natives.wuid_map ||
      !natives.report_unlocked || !natives.script_lock) {
    return false;
  }
  if (MH_CreateHook(natives.set_locked, reinterpret_cast<void*>(hooked_set_locked),
                    reinterpret_cast<void**>(&original_set_locked)) != MH_OK) {
    return false;
  }
  if (MH_CreateHook(natives.report_unlocked, reinterpret_cast<void*>(hooked_report_unlocked),
                    reinterpret_cast<void**>(&original_report_unlocked)) != MH_OK) {
    return false;
  }
  if (MH_EnableHook(natives.set_locked) != MH_OK ||
      MH_EnableHook(natives.report_unlocked) != MH_OK) {
    return false;
  }
  installed = true;
  return true;
}

void frame(void* local_actor, void* local_entity,
           const std::vector<kcd2s::integration::PilloryOrder>& orders, Reports& reports) {
  const ULONGLONG now = GetTickCount64();
  for (const auto& order : orders) {
    if (!installed) {
      // The feature is off in this build of the game: the server hears that nobody stands there.
      report_stay(reports, kcd2s::integration::kPilloryLeft);
      continue;
    }
    std::string holder = order.holder;
    if (order.kind == kcd2s::integration::kPilloryPutInNearest) {
      holder = nearest_holder(local_entity);
      if (holder.empty()) {
        say("PILLORY %s", "no pillory in this level to put the player into");
        wanted.clear();
        entered_unreported = false;
        report_stay(reports, kcd2s::integration::kPilloryLeft);
        continue;
      }
      say("PILLORY the nearest pillory is '%s'", holder.c_str());
      entered_unreported = true;
    } else if (order.kind == kcd2s::integration::kPilloryPutIn) {
      entered_unreported = false;
    }
    if (order.kind == kcd2s::integration::kPilloryRelease) {
      wanted.clear();
      entered_unreported = false;
    } else if (wanted != holder) {
      wanted = holder;
      wanted_since = now;
    }
  }
  if (!installed) {
    return;
  }
  if (lock_opened.exchange(false) && !inside.empty() && wanted == inside) {
    // Another player's lock pick (or a key) opened the pillory's lock: the stay is over.
    say("PILLORY the lock of '%s' was opened", inside.c_str());
    wanted.clear();
    lockpick_reported = true;
    report_stay(reports, kcd2s::integration::kPilloryLockpicked);
  }
  // Every pillory lock opened in this game, whoever is in a pillory: the server lets out the
  // players in the pillories of those locks.
  report_opened_locks(reports);
  if (!inside.empty() && wanted != inside) {
    try_take_out(now, reports);
  } else if (!wanted.empty() && inside.empty()) {
    try_put_in(now, local_actor, reports);
  }
  if (wanted.empty()) {
    entered_unreported = false;
  } else if (entered_unreported && wanted == inside) {
    // In the nearest pillory: the server learns its name, to send it again and to match its lock.
    report_entered(reports, inside);
    entered_unreported = false;
  }
  // The player stays in until the server or a lock pick lets them out: no Esc of their own.
  if (!inside.empty() && wanted == inside) {
    enable_unstance_map(false);
  }
}

bool in_stocks() {
  return !inside.empty();
}

void reset() {
  wanted.clear();
  // The world is gone and the unstance with it.
  forget_stay(false);
  lockpick_reported = false;
  entered_unreported = false;
  leaving_since = 0;
  std::lock_guard<std::mutex> guard(lock_mutex);
  remembered_lock = nullptr;
  remembered_holder.clear();
  opened_locks.clear();
}

}  // namespace kcds::runtime::pillory
