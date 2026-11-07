#include "background_mode.hpp"

#include <windows.h>

#include <atomic>
#include <thread>

#include "MinHook.h"

namespace kcds::runtime::background {
namespace {

Logger log_line = nullptr;

using SetForegroundWindowFn = BOOL(WINAPI*)(HWND);
using SetActiveWindowFn = HWND(WINAPI*)(HWND);
using BringWindowToTopFn = BOOL(WINAPI*)(HWND);
using ShowWindowFn = BOOL(WINAPI*)(HWND, int);
using SetWindowPosFn = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, void*);
using XInputGetCapabilitiesFn = DWORD(WINAPI*)(DWORD, DWORD, void*);
using XInputSetStateFn = DWORD(WINAPI*)(DWORD, void*);

SetForegroundWindowFn original_set_foreground = nullptr;
SetActiveWindowFn original_set_active = nullptr;
BringWindowToTopFn original_bring_to_top = nullptr;
ShowWindowFn original_show_window = nullptr;
SetWindowPosFn original_set_window_pos = nullptr;
XInputGetStateFn original_xinput_state = nullptr;
XInputGetCapabilitiesFn original_xinput_capabilities = nullptr;
XInputSetStateFn original_xinput_set_state = nullptr;

void note(const char* text) {
  if (log_line) {
    log_line("%s", text);
  }
}

// Set once someone clicks a window of this game with their own mouse: from then on it is their
// game, with the foreground and the controller as any game has them.
std::atomic<bool> taken_over{false};

BOOL WINAPI hooked_set_foreground(HWND window) {
  return taken_over.load() ? original_set_foreground(window) : TRUE;
}

HWND WINAPI hooked_set_active(HWND window) {
  return taken_over.load() ? original_set_active(window) : GetActiveWindow();
}

BOOL WINAPI hooked_bring_to_top(HWND window) {
  return taken_over.load() ? original_bring_to_top(window) : TRUE;
}

// Every way of showing that would activate shows without activating.
BOOL WINAPI hooked_show_window(HWND window, int command) {
  if (taken_over.load()) {
    return original_show_window(window, command);
  }
  switch (command) {
    case SW_SHOWNORMAL:
    case SW_SHOW:
    case SW_SHOWDEFAULT:
    case SW_SHOWMAXIMIZED:
      return original_show_window(window, SW_SHOWNA);
    case SW_RESTORE:
      return original_show_window(window, SW_SHOWNOACTIVATE);
    case SW_SHOWMINIMIZED:
      return original_show_window(window, SW_SHOWMINNOACTIVE);
    default:
      return original_show_window(window, command);
  }
}

BOOL WINAPI hooked_set_window_pos(HWND window, HWND after, int x, int y, int width, int height,
                                  UINT flags) {
  if (taken_over.load()) {
    return original_set_window_pos(window, after, x, y, width, height, flags);
  }
  if (after == HWND_TOPMOST) {
    after = HWND_NOTOPMOST;
  }
  return original_set_window_pos(window, after, x, y, width, height, flags | SWP_NOACTIVATE);
}

DWORD WINAPI hooked_xinput_state(DWORD user, void* state) {
  return taken_over.load() ? original_xinput_state(user, state) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI hooked_xinput_capabilities(DWORD user, DWORD flags, void* capabilities) {
  return taken_over.load() ? original_xinput_capabilities(user, flags, capabilities)
                           : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI hooked_xinput_set_state(DWORD user, void* vibration) {
  return taken_over.load() ? original_xinput_set_state(user, vibration)
                           : ERROR_DEVICE_NOT_CONNECTED;
}

// Whether the person at this PC is clicking a window of this game now: a mouse button is
// physically down with the pointer over it (a test kit's posted clicks press no button).
bool clicked_by_hand(HWND front) {
  const bool button =
      (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
  if (!button) {
    return false;
  }
  POINT cursor{};
  RECT rect{};
  return GetCursorPos(&cursor) && GetWindowRect(front, &rect) && PtInRect(&rect, cursor);
}

template <typename Function>
bool hook(const wchar_t* module, const char* name, void* detour, Function* original) {
  void* target = nullptr;
  if (MH_CreateHookApiEx(module, name, detour, reinterpret_cast<void**>(original), &target) !=
      MH_OK) {
    return false;
  }
  return MH_EnableHook(target) == MH_OK;
}

bool ours(HWND window) {
  DWORD process = 0;
  GetWindowThreadProcessId(window, &process);
  return process == GetCurrentProcessId();
}

// The foreground back to the window that had it whenever one of this game's comes to the front,
// until someone clicks it by hand: then it is theirs and the watch ends.
void watch_foreground() {
  HWND last_other = nullptr;
  unsigned given_back = 0;
  for (;;) {
    HWND front = GetForegroundWindow();
    if (front != nullptr && ours(front) && clicked_by_hand(front)) {
      taken_over.store(true);
      if (log_line) {
        log_line("BACKGROUND clicked by hand: this game keeps the foreground and the controller");
      }
      return;
    }
    if (front != nullptr && !ours(front)) {
      last_other = front;
    } else if (front != nullptr && last_other != nullptr && IsWindow(last_other)) {
      // This game is in front, so it may hand the foreground on.
      original_set_foreground(last_other);
      if (given_back < 5 && log_line) {
        log_line("BACKGROUND the foreground went back to the window that had it");
      }
      ++given_back;
    }
    Sleep(10);
  }
}

}  // namespace

bool enabled() {
#ifndef KCDS_DEV_BUILD
  // A released client is a player's game: never in the background, whatever the environment
  // holds (a player's game in this mode handed its foreground away, its cursor stayed).
  return false;
#else
  wchar_t value[8] = {};
  const DWORD length = GetEnvironmentVariableW(L"KCDS_BACKGROUND", value, 8);
  if (length == 1 && value[0] == L'1') {
    return true;
  }
  // The kits' automated runs name it KCDS_TEST_BACKGROUND (any value).
  return GetEnvironmentVariableW(L"KCDS_TEST_BACKGROUND", nullptr, 0) != 0;
#endif
}

void install(Logger logger) {
  if (!enabled()) {
    return;
  }
  log_line = logger;
  bool window_hooks =
      hook(L"user32", "SetForegroundWindow", reinterpret_cast<void*>(hooked_set_foreground),
           &original_set_foreground) &&
      hook(L"user32", "SetActiveWindow", reinterpret_cast<void*>(hooked_set_active),
           &original_set_active) &&
      hook(L"user32", "BringWindowToTop", reinterpret_cast<void*>(hooked_bring_to_top),
           &original_bring_to_top) &&
      hook(L"user32", "ShowWindow", reinterpret_cast<void*>(hooked_show_window),
           &original_show_window) &&
      hook(L"user32", "SetWindowPos", reinterpret_cast<void*>(hooked_set_window_pos),
           &original_set_window_pos);
  // WHGame imports XInput from XINPUT9_1_0, loaded with it later: loaded here first, so the
  // game's imports reach the hooked functions.
  LoadLibraryW(L"xinput9_1_0.dll");
  bool pad_hooks =
      hook(L"xinput9_1_0", "XInputGetState", reinterpret_cast<void*>(hooked_xinput_state),
           &original_xinput_state) &&
      hook(L"xinput9_1_0", "XInputGetCapabilities",
           reinterpret_cast<void*>(hooked_xinput_capabilities), &original_xinput_capabilities) &&
      hook(L"xinput9_1_0", "XInputSetState", reinterpret_cast<void*>(hooked_xinput_set_state),
           &original_xinput_set_state);
  if (original_set_foreground == nullptr) {
    original_set_foreground = &SetForegroundWindow;
  }
  std::thread(watch_foreground).detach();
  note(window_hooks ? "BACKGROUND no window of this game takes the foreground"
                    : "BACKGROUND window hooks incomplete; the foreground watch still runs");
  note(pad_hooks ? "BACKGROUND no controller: XInput answers 'not connected'"
                 : "BACKGROUND XInput hooks failed: this game may still read the controller");
}

}  // namespace kcds::runtime::background
