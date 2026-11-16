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
void* (*game_services)() = nullptr;
PhotoLogger log_line = nullptr;
// The photo mode manager (C_PhotomodeManager): the game services + 176, made with the game. Its
// byte + 561 is set while the photo mode is open: its action handlers use the photo camera only
// then (sub_180F8C26C).
constexpr std::size_t kServicesPhotoMode = 176;
constexpr std::size_t kPhotoModeOpen = 561;
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

std::uint8_t* photo_manager() {
  auto* services = game_services ? static_cast<std::uint8_t*>(game_services()) : nullptr;
  return services ? *reinterpret_cast<std::uint8_t**>(services + kServicesPhotoMode) : nullptr;
}

}  // namespace

bool photo_mode_open() {
  const std::uint8_t* manager = photo_manager();
  return manager && manager[kPhotoModeOpen] != 0;
}

bool install_photo_mode(void* (*game)(), void* (*services)(), void* left_photo_mode,
                        PhotoLogger logger) {
  game_singleton = game;
  game_services = services;
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
    // A photo mode open when the world was left stayed open in the game's eyes: the next world's
    // loading sent it the player's keys, and its handlers read a camera that was not there yet
    // (an access violation in the photo camera's getter). Out of a world it is closed.
    std::uint8_t* manager = photo_manager();
    if (manager && manager[kPhotoModeOpen] != 0) {
      manager[kPhotoModeOpen] = 0;
      if (log_line) {
        log_line("PHOTOMODE closed: the world was left with it open");
      }
    }
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
