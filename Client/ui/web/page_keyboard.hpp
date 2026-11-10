// The keyboard of a focused page, read on a thread of its own.
//
// A page's keys used to come through the game window's messages, which the game's main thread
// handles between its work: while it loads a level or stalls, a second or more of typing waited
// in the window's queue and then reached the page at once. While a page has the keyboard and the
// game's window is in front, this module's thread reads the keyboard's state every few
// milliseconds and hands each press, release and held-key repeat to the page as it happens,
// whatever the game's main thread is doing. The mouse's buttons and pointer go the same way (its
// wheel still comes with the window's messages). The input still goes on to the game's window,
// which does not act on it while a page has the keyboard.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace kcds::web::page_keyboard {

// A key as the keyboard has it.
struct Key {
  unsigned virtual_key = 0;
  unsigned scan_code = 0;
  bool extended = false;  // right Ctrl and Alt, the arrows, the keypad's Enter and slash, ...
  bool released = false;
  bool alt = false;  // Alt is held (a system key)
};

// Called on the module's thread for every key while a page has the keyboard.
using Sink = void (*)(const Key& key);
// Whether a page has the keyboard and the game's window is in front (any thread).
using Wanted = bool (*)();

// The mouse as the module's thread reads it: the pointer moved to `at` (screen pixels), or a
// button (VK_LBUTTON, VK_RBUTTON, VK_MBUTTON) went down or up there, `clicks` 1 or 2.
struct Mouse {
  POINT at{};
  int button = 0;  // 0: a move
  bool released = false;
  int clicks = 1;
};
using MouseSink = void (*)(const Mouse& mouse);

// Starts the module's thread (once).
void start(Sink sink, MouseSink mouse_sink, Wanted wanted);

// Whether the module's thread hands the keys to the page now: the game window's key messages are
// then not sent again.
bool active();

}  // namespace kcds::web::page_keyboard
