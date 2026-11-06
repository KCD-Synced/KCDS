// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "loading_tail.hpp"

#include <atomic>
#include <cstdint>

#include "../core/natives.hpp"
#include "MinHook.h"

namespace kcds::runtime::branding::loading_tail {
namespace {

using kcds::game_client::resolve_unique;

// The loading thread's start (its entry).
constexpr char kStartThread[] =
    "48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 60 65 48 8B 04 25 58 00 00 00 48 "
    "8B F1 8B 15 ?? ?? ?? ?? BD 18 00 00 00 48 8B 3C D0";
// The loading screen's system-event listener (its entry; cmp edx, 52 at + 27).
constexpr char kListener[] =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 60 49 8B F8 48 8B D9 83 FA 34 "
    "0F 8F";
// The listener is the screen's + 40; the screen's state (2: texture preload) and the loading
// thread's flag.
constexpr std::size_t kListenerOffset = 40;
constexpr std::size_t kScreenState = 368;
constexpr std::size_t kThreadStarted = 152;
constexpr int kLevelLoaded = 28;
// When the game's loading screen is drawn as it should be (its whole picture, hint and spinner):
// from the menu, event 19 shows it but its picture stays on its first, zoomed frame
// with no hint or spinner while the level loads (a frame every 0.5 s); it plays
// from event 28 on (the level is loaded, its thread started again below) until it ends.
std::atomic<bool> screen_up{false};
constexpr std::int32_t kPreloading = 2;

using StartThread = char(__fastcall*)(void* screen);
using Listener = void(__fastcall*)(void* listener, int event, std::int64_t parameter);

Logger log_line = nullptr;
StartThread start_thread = nullptr;
Listener original_listener = nullptr;

void __fastcall hooked_listener(void* listener, int event, std::int64_t parameter) {
  original_listener(listener, event, parameter);
  if (event == kLevelLoaded) {
    screen_up.store(true);
  }
  if (event != kLevelLoaded || !listener) {
    return;
  }
  auto* screen = static_cast<std::uint8_t*>(listener) - kListenerOffset;
  const std::int32_t state = *reinterpret_cast<std::int32_t*>(screen + kScreenState);
  if (state != kPreloading || screen[kThreadStarted] != 0) {
    return;
  }
  start_thread(screen);
  if (log_line) {
    log_line("LOADING screen keeps its spinner through the texture preload (thread %s)",
             screen[kThreadStarted] != 0 ? "started" : "not started");
  }
}

}  // namespace

bool level_screen_shown() {
  return screen_up.load() || !original_listener;
}

bool install(Logger logger) {
  log_line = logger;
  wchar_t value[4] = {};
  const DWORD length = GetEnvironmentVariableW(L"KCDS_LOADING_TAIL", value, 4);
  if (length > 0 && length < 4 && value[0] == L'0') {
    return false;
  }
  start_thread = reinterpret_cast<StartThread>(resolve_unique("LoadingThreadStart", kStartThread));
  void* listener = resolve_unique("LoadingScreenListener", kListener);
  const bool hooked = start_thread && listener &&
                      MH_CreateHook(listener, reinterpret_cast<void*>(&hooked_listener),
                                    reinterpret_cast<void**>(&original_listener)) == MH_OK &&
                      MH_EnableHook(listener) == MH_OK;
  if (logger) {
    logger("LOADING screen tail %s", hooked ? "installed" : "unavailable");
  }
  return hooked;
}

}  // namespace kcds::runtime::branding::loading_tail
