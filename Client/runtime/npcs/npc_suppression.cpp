#include "npc_suppression.hpp"

#include "../core/client_internal.hpp"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace kcds::runtime::npcs {
namespace {

// SEntitySpawnParams: the entity class at + 24; IEntityClass vtable slot 2 is its name.
constexpr std::size_t kSpawnClass = 24;
constexpr std::size_t kClassName = 2;
// The human NPC classes and the animal classes of the game's scripts (Scripts/Entities/AI), and
// the flocks and holders of animals on the ground (Scripts/Entities/Boids: chickens, rats, field
// mice; the cats' CatHolder). Live: a flock's chickens walked on one client only.
// Birds in the sky and fish stay the game's own scenery.
const char* const kRefusedClasses[] = {
    "NPC",           "NPC_Female",
    "NPC_NAI",       "Horse",
    "Dog",           "WildDog",
    "Wolf",          "Boar",
    "Pig",           "CattleBull",
    "CattleCow",     "SheepEwe",
    "SheepRam",      "Hare",
    "Hen",           "Raven",
    "RedDeerDoe",    "RedDeerStag",
    "RoeDeerBuck",   "RoeDeerHind",
    "Chickens",      "ChickensBrownLight",
    "ChickensWhite", "Rats",
    "FieldMouse",    "CatHolder",
};

using ClassName = const char*(__fastcall*)(void* klass);

Logger log_line = nullptr;
// The level's own people live (set_level_people), and their entities by catalog GUID.
std::atomic<bool> level_people{false};
std::mutex level_people_mutex;
std::unordered_map<std::string, std::uint32_t> level_people_entities;
std::atomic<unsigned> refused{0};
thread_local unsigned own_spawns = 0;

// SEntitySpawnParams: the entity's GUID at + 8 (two 64-bit words, as the level's EntityGuid),
// its name (a char pointer) at + 56 and its position at + 80.
constexpr std::size_t kSpawnGuid = 8;
constexpr std::size_t kSpawnName = 56;
constexpr std::size_t kSpawnLevelNode = 48;
constexpr std::size_t kSpawnPosition = 80;
// Test kits only (KCDS_TEST_HOOKS): every human NPC the game wanted to spawn, once per name, so
// the level's own people can be matched to their placements (issue 016). At most this many.
constexpr std::size_t kMaxLoggedPeople = 4000;
std::mutex logged_mutex;
std::unordered_set<std::string> logged_people;

// Copies up to 128 characters under a char pointer into `out`; their count, 0 when unreadable.
std::size_t copy_text(const char* chars, char* out) {
  std::size_t length = 0;
  __try {
    while (chars && length < 128 && chars[length] != '\0') {
      out[length] = chars[length];
      ++length;
    }
  } __except (1) {
    length = 0;
  }
  return length;
}

void log_refused_person(const void* params, const char* klass) {
  static const bool trace = GetEnvironmentVariableW(L"KCDS_TEST_HOOKS", nullptr, 0) != 0;
  if (!trace || !log_line ||
      (std::strncmp(klass, "NPC", 3) != 0 && std::strcmp(klass, "Dog") != 0)) {
    return;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(params);
  const char* name_chars = nullptr;
  std::memcpy(&name_chars, bytes + kSpawnName, sizeof(name_chars));
  char buffer[129] = {};
  const std::string name(buffer, copy_text(name_chars, buffer));
  {
    std::lock_guard<std::mutex> lock(logged_mutex);
    if (logged_people.size() >= kMaxLoggedPeople || !logged_people.insert(name).second) {
      return;
    }
  }
  std::uint64_t guid[2] = {};
  float position[3] = {};
  std::memcpy(guid, bytes + kSpawnGuid, sizeof(guid));
  std::memcpy(position, bytes + kSpawnPosition, sizeof(position));
  log_line("NPCS level person refused class=%s name=%s guid=%016llx:%016llx at=%.1f,%.1f,%.1f",
           klass, name.c_str(), static_cast<unsigned long long>(guid[0]),
           static_cast<unsigned long long>(guid[1]), position[0], position[1], position[2]);
}

const char* class_name_of(const void* params) {
  void* klass = nullptr;
  std::memcpy(&klass, static_cast<const std::uint8_t*>(params) + kSpawnClass, sizeof(klass));
  return klass ? (*reinterpret_cast<ClassName**>(klass))[kClassName](klass) : nullptr;
}

const char* refused_class(const void* params) {
  const char* name = class_name_of(params);
  if (!name) {
    return nullptr;
  }
  for (const char* refused_name : kRefusedClasses) {
    if (std::strcmp(name, refused_name) == 0) {
      return refused_name;
    }
  }
  return nullptr;
}

}  // namespace

void install(Logger logger) {
  log_line = logger;
}

bool spawn_class_is(const void* params, const char* name) {
  const char* actual = params ? class_name_of(params) : nullptr;
  return actual && std::strcmp(actual, name) == 0;
}

namespace {

// The human NPC classes the level spawns its people with.
bool person_class(const char* name) {
  return std::strcmp(name, "NPC") == 0 || std::strcmp(name, "NPC_Female") == 0;
}

// Whether a spawn comes from the level's files: the game's entity loader keeps the entity's XML
// node in the spawn (SEntitySpawnParams + 48), which a spawn made at run time (an animal spawner's)
// does not have.
bool from_level_file(const void* params) {
  void* node = nullptr;
  std::memcpy(&node, static_cast<const std::uint8_t*>(params) + kSpawnLevelNode, sizeof(node));
  return node != nullptr;
}

// The level's own people and the dogs it places: with their brain and the level's schedule,
// like its people (set_level_people). Dogs only from the level's files, not a spawner's.
bool level_class(const void* params, const char* name) {
  if (person_class(name)) {
    return true;
  }
  return std::strcmp(name, "Dog") == 0 && from_level_file(params);
}

// The catalog's text of a spawn's GUID (SEntitySpawnParams + 8): the low 32 bits, then the high
// 32 bits with their 16-bit halves swapped (the spawn's 42e1abf7002f4216 is the catalog's
// 002f4216abf742e1, as both list the same person).
std::string catalog_guid(const void* params) {
  std::uint64_t guid = 0;
  std::memcpy(&guid, static_cast<const std::uint8_t*>(params) + kSpawnGuid, sizeof(guid));
  const auto high = static_cast<std::uint32_t>(guid >> 32);
  const auto low = static_cast<std::uint32_t>(guid);
  const std::uint32_t swapped = (high << 16) | (high >> 16);
  char text[17] = {};
  sprintf_s(text, "%08x%08x", low, swapped);
  return text;
}

// The game's AI observer (issue 016). The AI's LOD manager keeps NPCs near its observer in full
// detail and puts the others in LOD or MonsterLOD, where C_FakeMovementManager
// moves the body in jumps without its walk. Its default observer is the player's actor, which this
// startup leaves unregistered (CryActorSystem's client id stays zero, frame.cpp), so people 20 m
// from the player were in MonsterLOD and walked in jumps a second apart, on every game.
// wh_ai_ObserverAdapterMode 2 makes the camera the observer ("0 -
// default observer, 1 - player observer, 2 - camera observer"). Set with the console's GetCVar
// (slot 23) and ICVar Set(text) (slot 9, 0x48), as the knock-out sets its fader variable.
// The variable is read (ICVar GetIVal, slot 2) every kObserverCheckMs and set again when the game
// gave it another value.
constexpr char kObserverMode[] = "wh_ai_ObserverAdapterMode";
constexpr char kCameraObserver[] = "2";
constexpr int kCameraObserverValue = 2;
constexpr std::size_t kConsoleGetCVar = 23;
constexpr std::size_t kCVarGetInt = 2;
constexpr std::size_t kCVarSetText = 9;
constexpr ULONGLONG kObserverCheckMs = 3000;
ULONGLONG next_observer_check = 0;
bool observer_unknown_logged = false;

}  // namespace

void keep_camera_observer() {
  const ULONGLONG now = GetTickCount64();
  if (now < next_observer_check) {
    return;
  }
  next_observer_check = now + kObserverCheckMs;
  void* console = kcds::game_client::console_slot ? *kcds::game_client::console_slot : nullptr;
  if (!console) {
    return;
  }
  using GetCVar = void*(__fastcall*)(void* console, const char* name);
  using GetInt = int(__fastcall*)(void* variable);
  using SetText = void(__fastcall*)(void* variable, const char* value);
  auto get = reinterpret_cast<GetCVar>((*static_cast<void***>(console))[kConsoleGetCVar]);
  void* variable = get(console, kObserverMode);
  if (!variable) {
    if (log_line && !observer_unknown_logged) {
      log_line("NPCS observer: %s unknown", kObserverMode);
    }
    observer_unknown_logged = true;
    return;
  }
  auto** table = *static_cast<void***>(variable);
  const int mode = reinterpret_cast<GetInt>(table[kCVarGetInt])(variable);
  if (mode == kCameraObserverValue) {
    return;
  }
  reinterpret_cast<SetText>(table[kCVarSetText])(variable, kCameraObserver);
  if (log_line) {
    log_line("NPCS observer: %s was %d, now %s", kObserverMode, mode, kCameraObserver);
  }
}

void set_level_people(bool on) {
  level_people = on;
  if (!on) {
    std::lock_guard<std::mutex> lock(level_people_mutex);
    level_people_entities.clear();
  }
}

void level_person_spawned(const void* params, void* entity) {
  if (!level_people || !params || !entity || own_spawns > 0) {
    return;
  }
  const char* name = class_name_of(params);
  if (!name || !level_class(params, name)) {
    return;
  }
  const auto id =
      *reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(entity) + 12);
  std::lock_guard<std::mutex> lock(level_people_mutex);
  level_people_entities[catalog_guid(params)] = id;
}

std::uint32_t level_person_entity(const std::string& guid) {
  std::lock_guard<std::mutex> lock(level_people_mutex);
  const auto found = level_people_entities.find(guid);
  return found == level_people_entities.end() ? 0 : found->second;
}

bool refuse_spawn(const void* params) {
  if (!params || own_spawns > 0) {
    return false;
  }
  const char* name = refused_class(params);
  if (!name) {
    return false;
  }
  // The level's own people and placed dogs live in this world: their spawns go through.
  if (level_people && level_class(params, name)) {
    return false;
  }
  log_refused_person(params, name);
  const unsigned count = ++refused;
  // The first refusal and then every thousandth, so the log shows it without filling up.
  if (log_line && (count == 1 || count % 1000 == 0)) {
    log_line(
        "NPCS spawn refused (%u so far, this one %s): NPCs and animals are off until they "
        "are synced",
        count, name);
  }
  return true;
}

OwnSpawn::OwnSpawn() {
  ++own_spawns;
}

OwnSpawn::~OwnSpawn() {
  --own_spawns;
}

}  // namespace kcds::runtime::npcs
