#include "page_keyboard.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <thread>

namespace kcds::web::page_keyboard {
namespace {

// How often the keyboard is read while a page has it, and how often otherwise (ms).
constexpr DWORD kReadEveryMs = 4;
constexpr DWORD kIdleEveryMs = 50;
// A held key types again after this long, then this often (ms), as Windows' own repeat does.
constexpr ULONGLONG kRepeatDelayMs = 500;
constexpr ULONGLONG kRepeatEveryMs = 33;

std::atomic<bool> started{false};
std::atomic<bool> reading{false};
Sink sink = nullptr;
MouseSink mouse_sink = nullptr;
Wanted wanted = nullptr;

// The mouse buttons read, and what the thread knows of them and of the pointer.
constexpr int kButtons[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON};
struct ButtonState {
  bool down = false;
  ULONGLONG pressed_at = 0;
  POINT pressed_where{};
  int clicks = 1;
};
struct MouseState {
  ButtonState buttons[3];
  POINT at{};
  bool known = false;
};

// Presses and releases of the mouse's buttons and the pointer's moves since the last read.
void read_mouse(MouseState& mouse, bool first) {
  POINT at{};
  if (!GetCursorPos(&at)) {
    return;
  }
  if (!first && (!mouse.known || at.x != mouse.at.x || at.y != mouse.at.y)) {
    Mouse move;
    move.at = at;
    mouse_sink(move);
  }
  mouse.at = at;
  mouse.known = true;
  const ULONGLONG now = GetTickCount64();
  for (int index = 0; index < 3; ++index) {
    ButtonState& button = mouse.buttons[index];
    const bool pressed = (GetAsyncKeyState(kButtons[index]) & 0x8000) != 0;
    if (first) {
      button.down = pressed;  // held when the page took the input: not a click
      continue;
    }
    if (pressed == button.down) {
      continue;
    }
    button.down = pressed;
    Mouse event;
    event.at = at;
    event.button = kButtons[index];
    event.released = !pressed;
    if (pressed) {
      // A second press soon after and near the first is a double click, as Windows counts it.
      const bool again =
          now - button.pressed_at <= GetDoubleClickTime() &&
          std::abs(at.x - button.pressed_where.x) <= GetSystemMetrics(SM_CXDOUBLECLK) &&
          std::abs(at.y - button.pressed_where.y) <= GetSystemMetrics(SM_CYDOUBLECLK);
      button.clicks = again && button.clicks == 1 ? 2 : 1;
      button.pressed_at = now;
      button.pressed_where = at;
    }
    event.clicks = button.clicks;
    mouse_sink(event);
  }
}

// Keys read (the thread's own): the generic Shift, Ctrl and Alt stand for their left and right
// keys, and the mouse buttons are not keys.
bool readable(int key) {
  if (key == VK_LBUTTON || key == VK_RBUTTON || key == VK_MBUTTON || key == VK_XBUTTON1 ||
      key == VK_XBUTTON2 || key == VK_CANCEL) {
    return false;
  }
  return key < VK_LSHIFT || key > VK_RMENU;
}

bool extended_key(int key) {
  switch (key) {
    case VK_RCONTROL:
    case VK_RMENU:
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_DIVIDE:
    case VK_NUMLOCK:
      return true;
    default:
      return false;
  }
}

void hand(int key, bool released) {
  Key event;
  event.virtual_key = static_cast<unsigned>(key);
  event.scan_code = MapVirtualKeyW(static_cast<UINT>(key), MAPVK_VK_TO_VSC) & 0xFF;
  event.extended = extended_key(key);
  event.released = released;
  event.alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
  sink(event);
}

bool is_modifier(int key) {
  return key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU || key == VK_CAPITAL ||
         key == VK_LWIN || key == VK_RWIN;
}

void run() {
  bool down[256] = {};
  ULONGLONG next_repeat[256] = {};
  MouseState mouse;
  for (;;) {
    const bool now_wanted = wanted && wanted();
    if (!now_wanted) {
      // Keys held when the page lost the keyboard are let go, so none is left pressed in it.
      if (reading.exchange(false)) {
        for (int key = 1; key < 256; ++key) {
          if (down[key]) {
            down[key] = false;
            hand(key, true);
          }
        }
      }
      Sleep(kIdleEveryMs);
      continue;
    }
    if (!reading.exchange(true)) {
      // Keys already held when the page took the keyboard are not typed again.
      for (int key = 1; key < 256; ++key) {
        down[key] = readable(key) && (GetAsyncKeyState(key) & 0x8000) != 0;
      }
      read_mouse(mouse, true);
    } else {
      read_mouse(mouse, false);
    }
    const ULONGLONG now = GetTickCount64();
    for (int key = 1; key < 256; ++key) {
      if (!readable(key)) {
        continue;
      }
      const bool pressed = (GetAsyncKeyState(key) & 0x8000) != 0;
      if (pressed && !down[key]) {
        down[key] = true;
        next_repeat[key] = now + kRepeatDelayMs;
        hand(key, false);
      } else if (!pressed && down[key]) {
        down[key] = false;
        hand(key, true);
      } else if (pressed && !is_modifier(key) && now >= next_repeat[key]) {
        next_repeat[key] = now + kRepeatEveryMs;
        hand(key, false);
      }
    }
    Sleep(kReadEveryMs);
  }
}

}  // namespace

void start(Sink key_sink, MouseSink page_mouse, Wanted page_wants_keys) {
  if (started.exchange(true)) {
    return;
  }
  sink = key_sink;
  mouse_sink = page_mouse;
  wanted = page_wants_keys;
  std::thread(run).detach();
}

bool active() {
  return reading.load();
}

}  // namespace kcds::web::page_keyboard
