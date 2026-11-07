#include "snow_stage.hpp"

#include <atomic>
#include <cstring>
#include <mutex>

#include "MinHook.h"

namespace kcds::runtime::winter::snow {
namespace {

// The stage's own copy of the snow parameters (see the header).
constexpr std::size_t kSnowInfo = 0x3EC0;
// The volume's radius around its centre: the whole world the camera sees.
constexpr float kRadius = 4000.0f;

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
  if (log_line && !told.exchange(true)) {
    log_line("WINTER snow drawn by the stage: amount %.2f, flakes %d", info.amount, info.flakes);
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

IsActive original_is_active = nullptr;

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

void set_console_int(const char* name, int value) {
  void* game_console = console ? *console : nullptr;
  if (!game_console) {
    return;
  }
  using GetCVar = void*(__fastcall*)(void* console, const char* name);
  using SetInt = void(__fastcall*)(void* variable, int value);
  void* variable = virtual_call<GetCVar>(game_console, 23)(game_console, name);
  if (!variable) {
    if (log_line) {
      log_line("WINTER %s unknown", name);
    }
    return;
  }
  virtual_call<SetInt>(variable, 7)(variable, value);
}

// The snow shaders are compiled by the game the first time they draw.
void allow_compiling() {
  if (compiling) {
    return;
  }
  set_console_int("r_ShadersAllowCompilation", 1);
  set_console_int("r_shadersWarningsAsErrorsWH", 0);
  compiling = true;
  if (log_line) {
    log_line("WINTER the game compiles the snow shaders");
  }
}

}  // namespace

bool install(void* update, void* is_active, void** console_slot, Logger logger) {
  log_line = logger;
  console = console_slot;
  if (original_update) {
    return true;
  }
  if (!update || !is_active) {
    return false;
  }
  const bool hooked = hook(update, reinterpret_cast<void*>(&snow_update),
                           reinterpret_cast<void**>(&original_update)) &&
                      hook(is_active, reinterpret_cast<void*>(&snow_is_active),
                           reinterpret_cast<void**>(&original_is_active));
  if (!hooked) {
    original_update = nullptr;
    original_is_active = nullptr;
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
  if (any) {
    allow_compiling();
  }
  on.store(any);
  told.store(false);
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
