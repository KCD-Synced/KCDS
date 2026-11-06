// Winsock before Windows.h (see client_internal.hpp): the game's console slot.
#include "../core/client_internal.hpp"

#include "studio_floor.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../core/client_state.hpp"
#include "../core/log.hpp"
#include "../native_ui/native_ui.hpp"
#include "../props/prop_entity.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "studio_floor_files.hpp"

namespace kcds::runtime::studio::floor {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

using GetCVar = void*(__fastcall*)(void* console, const char* name);
using CVarGetInt = int(__fastcall*)(void* cvar);
using CVarSetInt = void(__fastcall*)(void* cvar, int value);

constexpr std::size_t kConsoleGetCVar = 23;
constexpr std::size_t kCVarGetInt = 2;
constexpr std::size_t kCVarSetInt = 7;

template <class Fn>
Fn slot(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[index]);
}

constexpr const char* kEntityName = "kcds_studio_floor";
constexpr const char* kModel = "objects/kcds_studio/floor.cgf";

// -1: nothing asked; 0 or 1: the floor off or on.
std::atomic<int> wanted{-1};
// The kit's console variable requests (studioCVar): a name and a value (-9999: only read).
std::mutex cvar_mutex;
std::vector<std::pair<std::string, int>> cvar_requests;
bool culling_set = false;
// Effects in the studio: a map's level loads its particle libraries while it loads;
// the studio's level loads none, and afterwards the game's FindEffect loads a
// library only while e_ParticlesAllowRuntimeLoad (CVars+0x344) is on. Off, every
// effect is "not found" and nothing is drawn; on, its library loads at its first use.
bool particles_set = false;
bool look_ik_set = false;
bool feet_lock_set = false;
// The floor mesh: mounted and spawned once per world (tried again while it fails).
bool made = false;
bool shown = true;
int attempts = 0;
// The console variables the studio changed, with the values it found (given back on Leave).
std::vector<std::pair<std::string, int>> found_values;

// Mounts the embedded floor and spawns it (its square spans 0 to 128 m in its own space).
void make_floor() {
  if (props::entity::find(kEntityName)) {
    made = true;
    return;
  }
  using Bytes = std::vector<std::uint8_t>;
  std::vector<std::pair<std::string, Bytes>> entries;
  entries.emplace_back("kcds_studio/floor.cgf",
                       Bytes(std::begin(floor_files::kModel), std::end(floor_files::kModel)));
  entries.emplace_back("kcds_studio/floor.mtl",
                       Bytes(std::begin(floor_files::kMaterial), std::end(floor_files::kMaterial)));
  if (!native_ui::mount_files("_kcds/studio/floor", "objects/", entries)) {
    game_client::log("STUDIO floor: its pack could not be mounted");
    return;
  }
  const float origin[3] = {0.0f, 0.0f, 0.0f};
  made = props::entity::spawn_mesh(kEntityName, origin, kModel, "") != nullptr;
  game_client::log("STUDIO floor: %s", made ? "drawn" : "the game did not load it");
}

// Sets an int console variable; logs what it is afterwards. False without the console or it.
bool set_int(const char* name, int value) {
  void* console = game_client::console_slot ? *game_client::console_slot : nullptr;
  void* cvar = console ? slot<GetCVar>(console, kConsoleGetCVar)(console, name) : nullptr;
  if (!cvar) {
    game_client::log("STUDIO floor: no console variable %s", name);
    return false;
  }
  slot<CVarSetInt>(cvar, kCVarSetInt)(cvar, value);
  game_client::log("STUDIO floor: %s=%d", name, slot<CVarGetInt>(cvar, kCVarGetInt)(cvar));
  return true;
}

// set_int for the studio's own settings: the value found first is kept for Leave.
bool set_studio_int(const char* name, int value) {
  void* console = game_client::console_slot ? *game_client::console_slot : nullptr;
  void* cvar = console ? slot<GetCVar>(console, kConsoleGetCVar)(console, name) : nullptr;
  if (cvar) {
    bool kept = false;
    for (const auto& [kept_name, kept_value] : found_values) {
      kept = kept || kept_name == name;
    }
    if (!kept) {
      found_values.emplace_back(name, slot<CVarGetInt>(cvar, kCVarGetInt)(cvar));
    }
  }
  return set_int(name, value);
}

// The level's terrain drawn or not: the 3D engine's draw-the-terrain switch, which
// the game reads from the mission's EnvState ShowTerrainSurface when the level loads (C3DEngine
// +952). Changed only now, after the load, so the terrain keeps its physics (written
// as 0 in the level file, the level was not physicalized and the character fell). e_Terrain does
// not hide it. False (logged) when the engine or a plausible switch is not there.
constexpr std::size_t kShowTerrainSurface = 952;

bool show_terrain(bool on) {
  void* engine = game_client::engine_3d_slot ? *game_client::engine_3d_slot : nullptr;
  if (!engine) {
    game_client::log("STUDIO floor: no 3D engine for the terrain switch");
    return false;
  }
  auto* flag = static_cast<std::uint8_t*>(engine) + kShowTerrainSurface;
  if (*flag > 1) {
    game_client::log("STUDIO floor: the terrain switch reads %u, left alone", unsigned(*flag));
    return false;
  }
  *flag = on ? 1 : 0;
  game_client::log("STUDIO floor: the level's terrain %s", on ? "drawn" : "hidden");
  return true;
}

// An int console variable's value (logged); nothing without the console or it.
std::optional<int> read_int(const char* name) {
  void* console = game_client::console_slot ? *game_client::console_slot : nullptr;
  void* cvar = console ? slot<GetCVar>(console, kConsoleGetCVar)(console, name) : nullptr;
  if (!cvar) {
    game_client::log("STUDIO cvar %s: none", name);
    return std::nullopt;
  }
  const int value = slot<CVarGetInt>(cvar, kCVarGetInt)(cvar);
  game_client::log("STUDIO cvar %s is %d", name, value);
  return value;
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  const std::string operation = integration::script_operation(message);
  if (operation == "studioCVar") {
    // The kit's tests: an engine console variable set (value) or read (no value), logged.
    const sdk::Value* name = integration::script_field(message, "name");
    const sdk::Value* value = integration::script_field(message, "value");
    if (name && name->tag() == sdk::Tag::Str && name->as_str().size() < 64) {
      std::lock_guard<std::mutex> lock(cvar_mutex);
      cvar_requests.push_back(
          {name->as_str(), value && value->tag() == sdk::Tag::Int ? int(value->as_int()) : -9999});
    }
    return true;
  }
  if (operation != "studioFloor") {
    return false;
  }
  const sdk::Value* on = integration::script_field(message, "on");
  wanted.store(on && on->tag() == sdk::Tag::Bool && !on->as_bool() ? 0 : 1);
  return true;
}

void tick(const std::function<bool(const sdk::Value&)>& send) {
  std::vector<std::pair<std::string, int>> asked;
  {
    std::lock_guard<std::mutex> lock(cvar_mutex);
    asked.swap(cvar_requests);
  }
  for (const auto& [name, value] : asked) {
    if (value != -9999) {
      set_int(name.c_str(), value);
    }
    // Its value now to the editor (the assistant's studio_cvar), null without it.
    const std::optional<int> now = read_int(name.c_str());
    sdk::Value::Map answer{{"op", sdk::Value("studioCVarValue")}, {"name", sdk::Value(name)}};
    answer.emplace_back("value", now ? sdk::Value(std::int64_t(*now)) : sdk::Value());
    send(sdk::Value(std::move(answer)));
  }
  if (!culling_set) {
    culling_set = set_studio_int("e_TerrainOcclusionCulling", 0);
  }
  if (!particles_set) {
    particles_set = set_studio_int("e_ParticlesAllowRuntimeLoad", 1);
  }
  // The game's look IK turns the player's head toward its look target on top of any clip and
  // flips it when the target passes its limit: live, a game clip's head turned 107 degrees in a
  // frame and stayed twisted over the shoulder, at the same times every loop; with
  // ca_UseLookIK 0 none in a whole loop. The studio's character plays what is
  // animated, so the studio's world turns it off.
  if (!look_ik_set) {
    look_ik_set = set_studio_int("ca_UseLookIK", 0);
  }
  // The game locks the feet with IK under additive animations (ca_LockFeetWithIK, WHGame):
  // the studio's keys are additive (joint_turns), so a foot's keys did nothing and
  // an in-place clip stretched the leg after a planted foot. Off in the studio.
  if (!feet_lock_set) {
    feet_lock_set = set_studio_int("ca_LockFeetWithIK", 0);
  }
  if (!made && attempts < 3) {
    ++attempts;
    make_floor();
  }
  const int floor = wanted.exchange(-1);
  if (floor >= 0) {
    shown = floor == 1;
    set_studio_int("e_Terrain", floor);
    show_terrain(shown);
  }
  void* entity = made ? props::entity::find(kEntityName) : nullptr;
  if (entity && props::entity::is_hidden(entity) == shown) {
    props::entity::hide(entity, !shown);
  }
}

void leave() {
  for (const auto& [name, value] : found_values) {
    set_int(name.c_str(), value);
  }
  found_values.clear();
  culling_set = false;
  particles_set = false;
  look_ik_set = false;
  feet_lock_set = false;
  made = false;
  shown = true;
  attempts = 0;
  wanted.store(-1);
}

}  // namespace kcds::runtime::studio::floor
