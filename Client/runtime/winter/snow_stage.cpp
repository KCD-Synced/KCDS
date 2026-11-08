#include "snow_stage.hpp"

#include <atomic>
#include <cstring>
#include <mutex>

#include <windows.h>

#include <chrono>

#include <string>

#include "../physics/line_of_sight.hpp"
#include "../physics/local_world.hpp"
#include "MinHook.h"
#include "snow_cover.hpp"
#include "snow_shelter.hpp"

namespace kcds::runtime::winter::snow {
namespace {

// The stage's own copy of the snow parameters (see the header).
constexpr std::size_t kSnowInfo = 0x3EC0;
// The stage's render settings (+ 32) and their rain occlusion field (see snow_update).
constexpr std::size_t kStageSettingsOffset = 32;
constexpr std::size_t kSettingsRainOcclusion = 904;
// The settings' r_Snow, and its value while KCDS shows snow (2: with snow occlusion).
constexpr std::size_t kSettingsSnowValue = 940;
constexpr int kSnowSettingOn = 2;
// The settings' r_SnowDisplacement, kept off: its passes do not raise the snow, they shift the
// finished picture upward on the screen, which smeared everything near the camera.
constexpr std::size_t kSettingsSnowDisplacement = 944;
// The snow pass's stencil test as the game's code asks for it (see open_stencil): read mask 0x80
// ("mov r9b, 0x80" at + 2), the reference the same ("mov r8b, r9b" at + 8) and the test 6, equal
// (at + 12). The scene marks 0x80 on its rocks, buildings and ground, other values on its trees,
// bushes and brushwood, and leaves people at 0. KCDS's test: the whole stencil (mask 0xFF) not
// equal (7) to 0 ("mov r8b, 0"), snow on all of the world but its people.
constexpr std::size_t kSiteMask = 2;
constexpr std::size_t kSiteReference = 8;
constexpr std::size_t kSiteTest = 12;
constexpr std::uint8_t kGameMask = 0x80;
constexpr std::uint8_t kGameReference[3] = {0x45, 0x8A, 0xC1};
constexpr std::uint8_t kGameTest = 6;
constexpr std::uint8_t kSnowMask = 0xFF;
constexpr std::uint8_t kSnowReference[3] = {0x41, 0xB0, 0x00};
constexpr std::uint8_t kSnowTest = 7;

struct SnowInfo {
  float position[3];
  float radius;
  float amount;
  float frost;
  float freezing;
  std::int32_t flakes;
  float flake_size;
  float brightness;
  float gravity;
  float wind;
  float turbulence;
  float turbulence_frequency;
};
static_assert(sizeof(SnowInfo) == 56, "the stage keeps 56 bytes of snow");

using Update = void(__fastcall*)(void* stage);

Logger log_line = nullptr;
Update original_update = nullptr;
std::mutex mutex;
Settings wanted;
// The middle of a 4 km map until the camera's place is known.
float centre[3] = {2048.0f, 2048.0f, 100.0f};
// The roof over the camera (snow_shelter.hpp), measured a few times a second.
snow_shelter::Shelter sheltered;
// The snow volume's radius: the whole world the camera sees.
constexpr float kVolumeRadius = 4000.0f;
// How often the roof is measured, and how high a roof is looked for over the camera.
constexpr auto kShelterInterval = std::chrono::milliseconds(250);
constexpr float kRoofHighest = 30.0f;
std::chrono::steady_clock::time_point next_shelter;
// Where the roof was last measured, and how far the camera moves before it is measured again.
float measured_at[3] = {0.0f, 0.0f, -100000.0f};
constexpr float kShelterMove = 0.3f;
std::atomic<bool> on{false};
std::atomic<bool> told{false};

void __fastcall snow_update(void* stage) {
  original_update(stage);
  if (!on.load()) {
    return;
  }
  SnowInfo info{};
  {
    std::lock_guard<std::mutex> lock(mutex);
    // The volume: larger than the world, so the pass draws the whole screen, its centre the
    // box without snow (snow_cover.hpp): x, y the roof's middle and z the box's code, or 0 in the
    // open.
    info.radius = kVolumeRadius;
    if (sheltered.covered) {
      info.position[0] = sheltered.centre[0];
      info.position[1] = sheltered.centre[1];
      info.position[2] =
          snow_cover::box_code(sheltered.half_length, sheltered.half_width, sheltered.turn);
    } else {
      info.position[0] = centre[0];
      info.position[1] = centre[1];
      info.position[2] = 0.0f;
    }
    info.amount = wanted.amount;
    info.frost = wanted.frost;
    info.freezing = wanted.freezing;
    // No flakes fall under a roof: the flakes already falling stay until their size and brightness
    // are 0 (the count only stops new ones).
    info.flakes = sheltered.covered ? 0 : wanted.flakes;
    info.flake_size = sheltered.covered ? 0.0f : wanted.flake_size;
    info.brightness = sheltered.covered ? 0.0f : wanted.brightness;
    info.gravity = wanted.gravity;
    info.wind = wanted.wind;
    info.turbulence = wanted.turbulence;
    info.turbulence_frequency = wanted.turbulence_frequency;
  }
  std::memcpy(static_cast<std::uint8_t*>(stage) + kSnowInfo, &info, sizeof(info));
  // The falling flakes' pass draws only inside the rain's occlusion volume while the settings'
  // rain occlusion field (+ 904, made by the game, not a console setting) is set, and without rain
  // that volume is empty: while flakes fall it is cleared for this frame's passes.
  auto* stage_settings =
      *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(stage) + kStageSettingsOffset);
  if (info.flakes > 0 && stage_settings) {
    *reinterpret_cast<int*>(stage_settings + kSettingsRainOcclusion) = 0;
  }
  // The settings' r_Snow for this frame's passes (the flakes' clusters, the displacement read it
  // there; the console's value reaches the settings only when the pipeline takes its settings).
  // What the renderer's settings held for r_Snow before (logged when it changes).
  static int snow_setting_seen = -1;
  const int snow_setting =
      stage_settings ? *reinterpret_cast<int*>(stage_settings + kSettingsSnowValue) : -1;
  if (stage_settings) {
    *reinterpret_cast<int*>(stage_settings + kSettingsSnowValue) = kSnowSettingOn;
    *reinterpret_cast<int*>(stage_settings + kSettingsSnowDisplacement) = 0;
  }
  if (log_line && snow_setting != snow_setting_seen) {
    snow_setting_seen = snow_setting;
    log_line("WINTER the renderer's settings hold r_Snow %d", snow_setting);
  }
  // The rain the stage copied (+ 15776): its amount (+ 192) and whether its occlusion map is
  // applied (+ 264), which the snow's pass needs to keep snow out from under roofs.
  {
    auto* bytes = static_cast<std::uint8_t*>(stage);
    static float rain_seen = -1.0f;
    static int occlusion_seen = -1;
    const float rain = *reinterpret_cast<float*>(bytes + 15776 + 192);
    const int occlusion = bytes[15776 + 264];
    if (log_line && (rain != rain_seen || occlusion != occlusion_seen)) {
      rain_seen = rain;
      occlusion_seen = occlusion;
      log_line("WINTER rain for the snow: amount %.3f, occlusion %d (bytes %02x %02x %02x %02x)",
               rain, occlusion, bytes[15776 + 260], bytes[15776 + 261], bytes[15776 + 262],
               bytes[15776 + 263]);
    }
  }
  // The flakes' clusters the pass made (+ 16164): set once the falling flakes' pass ran.
  static int clusters_seen = -1;
  const int clusters = *reinterpret_cast<int*>(static_cast<std::uint8_t*>(stage) + 16164);
  if (log_line && clusters != clusters_seen) {
    clusters_seen = clusters;
    log_line("WINTER snow flakes: %d clusters made by the flakes' pass", clusters);
  }
  if (log_line && !told.exchange(true)) {
    // What the falling flakes' pass asks besides r_snow: the rain settings' field (+ 904, its
    // rain occlusion test when set), and the flakes' clusters the stage made (+ 16164).
    auto* bytes = static_cast<std::uint8_t*>(stage);
    auto* settings = *reinterpret_cast<std::uint8_t**>(bytes + 32);
    log_line(
        "WINTER snow drawn by the stage: amount %.2f, flakes %d (rain field %d, clusters %d, "
        "rain %.2f / %.2f)",
        info.amount, info.flakes, settings ? *reinterpret_cast<int*>(settings + 904) : -1,
        *reinterpret_cast<int*>(bytes + 16164), *reinterpret_cast<float*>(bytes + 15920),
        *reinterpret_cast<float*>(bytes + 15932));
  }
}

// The stage's switch: the pipeline's render settings (stage + 32) hold r_snow at + 940, copied
// once when the pipeline is built (the game's is 0, whatever the console says), and the stage runs
// only while it is above 0 and post effects (+ 584) are on. When snow is wanted the render
// thread turns it on as the engine does on a change of r_snow: the setting, then the stage's
// own init (vtable 1: the snowflake, noise and frost textures) and resize (vtable 2: the
// displacement target at the pipeline's size, + 40 -> + 36208 / + 36212).
constexpr std::size_t kStageSettings = 32;
constexpr std::size_t kStagePipeline = 40;
constexpr std::size_t kSettingsSnow = 940;
constexpr std::size_t kPipelineWidth = 36208;
constexpr std::size_t kPipelineHeight = 36212;
constexpr int kSnowWithOcclusion = 2;

using IsActive = bool(__fastcall*)(void* stage);
using StageInit = void(__fastcall*)(void* stage);
using StageResize = void(__fastcall*)(void* stage, int width, int height);
using SnowEnabled = bool(__fastcall*)(void* settings);

IsActive original_is_active = nullptr;
SnowEnabled original_snow_enabled = nullptr;

// The settings' own "r_snow above 0" question, which the falling flakes' pass asks (and the
// stage's init): yes while KCDS shows snow, as the stage's settings keep r_snow 0.
bool __fastcall snow_enabled(void* settings) {
  return on.load() || original_snow_enabled(settings);
}

template <class Fn>
Fn virtual_call(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*static_cast<void***>(object))[index]);
}

// The stage whose resources KCDS made (render thread only).
void* prepared = nullptr;

bool __fastcall snow_is_active(void* stage) {
  if (!on.load()) {
    return original_is_active(stage);
  }
  auto* bytes = static_cast<std::uint8_t*>(stage);
  auto* settings = *reinterpret_cast<std::uint8_t**>(bytes + kStageSettings);
  auto* pipeline = *reinterpret_cast<std::uint8_t**>(bytes + kStagePipeline);
  if (!settings || !pipeline) {
    return original_is_active(stage);
  }
  if (prepared != stage) {
    // The init reads r_snow from the settings; the settings are given back at once, so the
    // pipeline sees no change of r_snow (a change to 0 would let the resources go again).
    int& snow = *reinterpret_cast<int*>(settings + kSettingsSnow);
    const int was = snow;
    snow = kSnowWithOcclusion;
    virtual_call<StageInit>(stage, 1)(stage);
    virtual_call<StageResize>(stage, 2)(stage, *reinterpret_cast<int*>(pipeline + kPipelineWidth),
                                        *reinterpret_cast<int*>(pipeline + kPipelineHeight));
    snow = was;
    prepared = stage;
    if (log_line) {
      log_line("WINTER snow stage prepared (%dx%d)",
               *reinterpret_cast<int*>(pipeline + kPipelineWidth),
               *reinterpret_cast<int*>(pipeline + kPipelineHeight));
    }
  }
  return true;
}

bool hook(void* target, void* detour, void** original) {
  return target && MH_CreateHook(target, detour, original) == MH_OK &&
         MH_EnableHook(target) == MH_OK;
}

// The game's console (IConsole: slot 23 GetCVar; ICVar slot 7 Set(int)).
void** console = nullptr;
bool compiling = false;

// Sets a console variable; false while the game has no console yet (the snow can come with the
// join, before the game made it) or the variable is unknown.
bool set_console_int(const char* name, int value) {
  void* game_console = console ? *console : nullptr;
  if (!game_console) {
    return false;
  }
  using GetCVar = void*(__fastcall*)(void* console, const char* name);
  using SetInt = void(__fastcall*)(void* variable, int value);
  void* variable = virtual_call<GetCVar>(game_console, 23)(game_console, name);
  if (!variable) {
    if (log_line) {
      log_line("WINTER %s unknown", name);
    }
    return false;
  }
  virtual_call<SetInt>(variable, 7)(variable, value);
  if (log_line) {
    using GetInt = int(__fastcall*)(void* variable);
    log_line("WINTER %s = %d (reads %d)", name, value, virtual_call<GetInt>(variable, 2)(variable));
  }
  return true;
}

int get_console_int(const char* name, int fallback) {
  void* game_console = console ? *console : nullptr;
  if (!game_console) {
    return fallback;
  }
  using GetCVar = void*(__fastcall*)(void* console, const char* name);
  using GetInt = int(__fastcall*)(void* variable);
  void* variable = virtual_call<GetCVar>(game_console, 23)(game_console, name);
  return variable ? virtual_call<GetInt>(variable, 2)(variable) : fallback;
}

// The snow's pass draws only where the scene marked its stencil (the level's rocks, buildings
// and ground), never on grass, so the ground under the grass stayed green. Deep snow buries the
// grass: while this much snow lies the game's grass (its merged meshes) is not drawn, and the
// player's own setting comes back when the snow goes.
constexpr float kGrassBuried = 4.0f;
constexpr const char* kGrassSetting = "e_MergedMeshes";
bool grass_hidden = false;
int grass_setting = 1;

void bury_grass(bool buried) {
  if (buried == grass_hidden) {
    return;
  }
  if (buried) {
    const int setting = get_console_int(kGrassSetting, 1);
    if (!set_console_int(kGrassSetting, 0)) {
      return;
    }
    grass_setting = setting;
  } else if (!set_console_int(kGrassSetting, grass_setting)) {
    return;
  }
  grass_hidden = buried;
}

// The snow shaders are compiled by the game the first time they draw.
void allow_compiling() {
  if (compiling) {
    return;
  }
  // The renderer's settings take r_Snow from the console each frame, and several of the snow's
  // passes (the falling flakes' clusters) read it there.
  const bool done = set_console_int("r_ShadersAllowCompilation", 1) &&
                    set_console_int("r_shadersWarningsAsErrorsWH", 0) &&
                    set_console_int("r_Snow", kSnowWithOcclusion) &&
                    set_console_int("r_SnowDisplacement", 0);
  if (!done) {
    return;
  }
  compiling = true;
  if (log_line) {
    log_line("WINTER the game compiles the snow shaders");
  }
}

// The console's part of the snow wanted (main thread): again each frame until the console took it.
std::atomic<bool> console_wanted{false};
std::atomic<bool> grass_wanted{false};

void apply_console() {
  if (console_wanted.load()) {
    allow_compiling();
  }
  bury_grass(grass_wanted.load());
}

}  // namespace

// The stencil test the snow's pass asks for, written as KCDS's in the game's code (`site`: the
// call's arguments, see kSiteMask): that code runs only while the snow stage does.
void open_stencil(std::uint8_t* site) {
  const bool as_shipped = site && site[kSiteMask] == kGameMask && site[kSiteTest] == kGameTest &&
                          std::memcmp(site + kSiteReference, kGameReference, 3) == 0;
  if (!as_shipped) {
    if (log_line) {
      log_line("WINTER snow stencil left as it is");
    }
    return;
  }
  constexpr std::size_t kLength = kSiteTest + 1;
  DWORD old_protection = 0;
  if (!VirtualProtect(site, kLength, PAGE_EXECUTE_READWRITE, &old_protection)) {
    return;
  }
  site[kSiteMask] = kSnowMask;
  std::memcpy(site + kSiteReference, kSnowReference, 3);
  site[kSiteTest] = kSnowTest;
  DWORD ignored = 0;
  VirtualProtect(site, kLength, old_protection, &ignored);
  FlushInstructionCache(GetCurrentProcess(), site, kLength);
  if (log_line) {
    log_line("WINTER snow falls on trees and bushes too, not on people (stencil not 0)");
  }
}

bool install(void* update, void* is_active, void* enabled, void* stencil_site, void** console_slot,
             Logger logger) {
  log_line = logger;
  console = console_slot;
  open_stencil(static_cast<std::uint8_t*>(stencil_site));
  if (original_update) {
    return true;
  }
  if (!update || !is_active || !enabled) {
    return false;
  }
  const bool hooked = hook(update, reinterpret_cast<void*>(&snow_update),
                           reinterpret_cast<void**>(&original_update)) &&
                      hook(is_active, reinterpret_cast<void*>(&snow_is_active),
                           reinterpret_cast<void**>(&original_is_active)) &&
                      hook(enabled, reinterpret_cast<void*>(&snow_enabled),
                           reinterpret_cast<void**>(&original_snow_enabled));
  if (!hooked) {
    original_update = nullptr;
    original_is_active = nullptr;
    original_snow_enabled = nullptr;
  }
  return hooked;
}

void set(const Settings& settings) {
  {
    std::lock_guard<std::mutex> lock(mutex);
    wanted = settings;
  }
  const bool any = settings.amount > 0.0f || settings.frost > 0.0f || settings.freezing > 0.0f ||
                   settings.flakes > 0;
  console_wanted.store(any);
  grass_wanted.store(settings.amount >= kGrassBuried);
  apply_console();
  on.store(any);
  told.store(false);
}

void settle() {
  apply_console();
}

void shelter(const float camera[3]) {
  if (!on.load()) {
    // Measured afresh when the snow comes back.
    measured_at[2] = -100000.0f;
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now < next_shelter) {
    return;
  }
  next_shelter = now + kShelterInterval;
  // Measured again only when the camera moved: under a roof a measure is up to 256 rays.
  const float moved[3] = {camera[0] - measured_at[0], camera[1] - measured_at[1],
                          camera[2] - measured_at[2]};
  if (moved[0] * moved[0] + moved[1] * moved[1] + moved[2] * moved[2] <
      kShelterMove * kShelterMove) {
    return;
  }
  measured_at[0] = camera[0];
  measured_at[1] = camera[1];
  measured_at[2] = camera[2];
  // A roof is what a building or the rock is made of: a tree's crown, a person or a horse
  // overhead is none, and neither is the ground, which a ray from a point on a slope starts under.
  // Only the roof over the camera is measured (the first point asked): the next house's roof
  // stretched the box over the yard between them.
  void* camera_roof = nullptr;
  const auto roof_over = [&camera_roof](float x, float y, float z) {
    const float from[3] = {x, y, z + 0.2f};
    const float to[3] = {x, y, z + kRoofHighest};
    physics::RayHit hit;
    if (physics::trace(from, to, physics::kHitCover, nullptr, 0, hit) !=
            physics::TraceResult::Hit ||
        !physics::hit_is_building(hit)) {
      return false;
    }
    if (!camera_roof) {
      camera_roof = hit.collider;
    }
    return hit.collider == camera_roof;
  };
  const auto started = std::chrono::steady_clock::now();
  const snow_shelter::Shelter measured = snow_shelter::shelter_at(camera, roof_over);
  const double took_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    changed = measured.covered != sheltered.covered;
    sheltered = measured;
    centre[0] = camera[0];
    centre[1] = camera[1];
    centre[2] = camera[2];
  }
  if (log_line && changed) {
    log_line(
        "WINTER camera %s (box at %.1f, %.1f, %.1f by %.1f m, turned %.0f deg; measured in "
        "%.2f ms)",
        measured.covered ? "under a roof: no snow there" : "in the open", measured.centre[0],
        measured.centre[1], measured.half_length * 2.0f, measured.half_width * 2.0f,
        measured.turn * 57.2957795f, took_ms);
  }
}

void follow(float x, float y, float z) {
  std::lock_guard<std::mutex> lock(mutex);
  centre[0] = x;
  centre[1] = y;
  centre[2] = z;
}

bool active() {
  return on.load();
}

}  // namespace kcds::runtime::winter::snow
