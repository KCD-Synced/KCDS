#include "window_icon.hpp"

#include <chrono>
#include <mutex>
#include <set>
#include <thread>

#include "assets/kcds_game_client_resources.h"

namespace kcds::runtime::branding::window_icon {
namespace {

// Sizes drawn from the icon: the small one for the title bar, the big one for the taskbar and
// Alt+Tab (the system scales it down where it needs less).
constexpr int kBigSide = 64;
// The watch looks for the process's new windows this often, for this long from the start (the
// splash, then the game's window, both within the first seconds).
constexpr auto kWatchEvery = std::chrono::milliseconds(15);
constexpr auto kWatchFor = std::chrono::minutes(3);

std::mutex applied_mutex;
std::set<HWND> applied;
HICON big_icon = nullptr;
HICON small_icon = nullptr;

// The client DLL, whose resources hold the icon.
HMODULE own_module() {
  HMODULE module = nullptr;
  GetModuleHandleExW(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&own_module), &module);
  return module;
}

HICON load_icon(int side) {
  return static_cast<HICON>(LoadImageW(own_module(), MAKEINTRESOURCEW(IDI_KCDS_GAME_CLIENT),
                                       IMAGE_ICON, side, side, LR_DEFAULTCOLOR));
}

// The icons, loaded once; they stay for the process's life (a window may show them at any time).
bool icons_loaded() {
  if (!big_icon) {
    big_icon = load_icon(kBigSide);
  }
  if (!small_icon) {
    small_icon = load_icon(GetSystemMetrics(SM_CXSMICON));
  }
  return big_icon && small_icon;
}

BOOL CALLBACK each_window(HWND window, LPARAM log) {
  DWORD process = 0;
  GetWindowThreadProcessId(window, &process);
  if (process == GetCurrentProcessId()) {
    apply(window, reinterpret_cast<Logger>(log));
  }
  return TRUE;
}

}  // namespace

void apply(HWND window, Logger log) {
  if (!window) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(applied_mutex);
    if (!applied.insert(window).second) {
      return;
    }
  }
  if (!icons_loaded()) {
    if (log) {
      log("BRANDING window icon not loaded (error %lu)", GetLastError());
    }
    return;
  }
  // The class's icons first: the game's windows made from it later start with ours.
  SetClassLongPtrW(window, GCLP_HICON, reinterpret_cast<LONG_PTR>(big_icon));
  SetClassLongPtrW(window, GCLP_HICONSM, reinterpret_cast<LONG_PTR>(small_icon));
  PostMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_icon));
  PostMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
  if (log) {
    log("BRANDING window icon set (window %p)", static_cast<void*>(window));
  }
}

void watch(Logger log) {
  std::thread([log]() {
    const auto until = std::chrono::steady_clock::now() + kWatchFor;
    while (std::chrono::steady_clock::now() < until) {
      EnumWindows(&each_window, reinterpret_cast<LPARAM>(log));
      std::this_thread::sleep_for(kWatchEvery);
    }
  }).detach();
}

}  // namespace kcds::runtime::branding::window_icon
