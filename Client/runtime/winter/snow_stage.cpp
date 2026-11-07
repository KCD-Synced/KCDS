#include "snow_stage.hpp"

#include <atomic>
#include <cstring>
#include <mutex>

#include <windows.h>

#include "MinHook.h"

namespace kcds::runtime::winter::snow {
namespace {

// The stage's own copy of the snow parameters (see the header).
constexpr std::size_t kSnowInfo = 0x3EC0;
// The volume's radius around its centre: the whole world the camera sees.
constexpr float kRadius = 4000.0f;
// The stage's render settings (+ 32) and their rain occlusion field (see snow_update).
constexpr std::size_t kStageSettingsOffset = 32;
constexpr std::size_t kSettingsRainOcclusion = 904;
// The settings' r_Snow, and its value while KCDS shows snow (2: with snow occlusion).
constexpr std::size_t kSettingsSnowValue = 940;
constexpr int kSnowSettingOn = 2;
// The settings' r_SnowDisplacement, kept off: its passes do not raise the snow, they shift the
// finished picture upward on the screen, which smeared everything near the camera.
constexpr std::size_t kSettingsSnowDisplacement = 944;
// The snow pass's stencil test as the game sets it each frame: 6 (equal), against bit 0x80, which
// the scene marks on its rocks, buildings and ground but never on trees, bushes or brushwood.
// KCDS's: 0 (always), snow on every face that looks up (see install).
constexpr std::uint8_t kStencilEqual = 6;
constexpr std::uint8_t kStencilAlways = 0;

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
    std::memcpy(info.position, centre, sizeof(centre));
    info.radius = kRadius;
    info.amount = wanted.amount;
    info.frost = wanted.frost;
    info.freezing = wanted.freezing;
    info.flakes = wanted.flakes;
    info.flake_size = wanted.flake_size;
    info.brightness = wanted.brightness;
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

// The stencil test the snow's pass asks for (the 6 in its "mov edx, 6"), made "always" in the
// game's code: that code runs only while the snow stage does.
void open_stencil(std::uint8_t* operand) {
  if (!operand || *operand != kStencilEqual) {
    if (log_line) {
      log_line("WINTER snow stencil left as it is (%d)", operand ? int(*operand) : -1);
    }
    return;
  }
  DWORD old_protection = 0;
  if (!VirtualProtect(operand, 1, PAGE_EXECUTE_READWRITE, &old_protection)) {
    return;
  }
  *operand = kStencilAlways;
  DWORD ignored = 0;
  VirtualProtect(operand, 1, old_protection, &ignored);
  FlushInstructionCache(GetCurrentProcess(), operand, 1);
  if (log_line) {
    log_line("WINTER snow falls on trees and bushes too (stencil always)");
  }
}

bool install(void* update, void* is_active, void* enabled, void* stencil_operand,
             void** console_slot, Logger logger) {
  log_line = logger;
  console = console_slot;
  open_stencil(static_cast<std::uint8_t*>(stencil_operand));
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
