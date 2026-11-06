// The game window's own icon: the KCDS mark in place of the game's on
// the taskbar, in Alt+Tab and in the title bar while the client runs. Only the running window
// changes (WM_SETICON and its class's icons): no game file is touched, so the game started
// without KCDS keeps its own icon. The icon is the KCDS one the launcher has (assets/kcds.ico),
// built into the client DLL's resources (assets/kcds-game-client.rc).
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace kcds::runtime::branding::window_icon {

using Logger = void (*)(const char* format, ...);

// Gives the window the KCDS icon, once per window. Any thread: the window gets it by posted
// messages, so a busy game thread is never waited for.
void apply(HWND window, Logger log);

// From the client's start: every window of the game's process gets the icon the moment it
// appears (the splash, then the game's own window), so the taskbar shows the KCDS icon from the
// first frame. A thread looks every 15 ms for the first three minutes.
void watch(Logger log);

}  // namespace kcds::runtime::branding::window_icon
