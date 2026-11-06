#include "photo_mode.hpp"

#include <cstddef>
#include <cstdint>

#include "MinHook.h"

namespace kcds::runtime::camera {
namespace {

using GetActionMapManager = void*(__fastcall*)(void* framework);
using EnableActionMap = void(__fastcall*)(void* manager, const char* name, bool enable);

constexpr std::size_t kGameFramework = 8;
constexpr std::size_t kFrameworkActionMapManager = 28;
constexpr std::size_t kEnableActionMap = 29;
constexpr char kPhotoModeMap[] = "photomode_enable";
// How often a map the game turned on again is put back off, in seconds.
constexpr double kReapplySeconds = 1;

void* (*game_singleton)() = nullptr;
PhotoLogger log_line = nullptr;
bool allowed = true;
bool applied_off = false;
double next_apply = 0;

using LeftPhotoMode = void(__fastcall*)(void* unused);
LeftPhotoMode original_left_photo_mode = nullptr;

// The remark the game makes when the photo mode closes: not in a KCDS game.
void __fastcall skipped_left_photo_mode(void*) {
  if (log_line) {
    log_line("PHOTOMODE closed: the character's remark skipped");
  }
}

template <class Fn>
Fn slot(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[index]);
}

void enable_map(bool enable) {
  auto* game = game_singleton ? static_cast<std::uint8_t*>(game_singleton()) : nullptr;
  void* framework = game ? *reinterpret_cast<void**>(game + kGameFramework) : nullptr;
  void* manager = framework
                      ? slot<GetActionMapManager>(framework, kFrameworkActionMapManager)(framework)
                      : nullptr;
  if (manager) {
    slot<EnableActionMap>(manager, kEnableActionMap)(manager, kPhotoModeMap, enable);
  }
}

}  // namespace

bool install_photo_mode(void* (*game)(), void* left_photo_mode, PhotoLogger logger) {
  game_singleton = game;
  log_line = logger;
  if (left_photo_mode) {
    const bool hooked =
        MH_CreateHook(left_photo_mode, reinterpret_cast<void*>(&skipped_left_photo_mode),
                      reinterpret_cast<void**>(&original_left_photo_mode)) == MH_OK &&
        MH_EnableHook(left_photo_mode) == MH_OK;
    if (log_line) {
      log_line("PHOTOMODE close remark %s", hooked ? "skipped" : "not hooked");
    }
  }
  return game_singleton != nullptr;
}

void set_photo_mode_allowed(bool now_allowed) {
  if (allowed == now_allowed) {
    return;
  }
  allowed = now_allowed;
  next_apply = 0;
  if (log_line) {
    log_line("PHOTOMODE %s by the server", allowed ? "allowed" : "not allowed");
  }
}

void tick_photo_mode(bool in_world, double seconds, bool knocked_out) {
  if (!game_singleton) {
    return;
  }
  if (!in_world) {
    // Out of a world the server's word is gone; the next server says again when it is off.
    allowed = true;
  }
  if (allowed && !knocked_out) {
    if (applied_off) {
      applied_off = false;
      enable_map(true);
    }
    return;
  }
  if (seconds < next_apply) {
    return;
  }
  next_apply = seconds + kReapplySeconds;
  applied_off = true;
  enable_map(false);
}

}  // namespace kcds::runtime::camera
