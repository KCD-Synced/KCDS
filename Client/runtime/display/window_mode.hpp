// The borderless window in the game's own Settings > Graphics > Window mode.
//
// The engine's r_WindowType has a borderless window of the chosen size (1) that the game's menu
// does not list; its Fullscreen is the borderless full screen (2) and Windowed a normal window (0).
// KCDS adds the borderless window as a third choice of the game's own row. The game applies a
// choice by its value, so choosing it sets r_WindowType 1 through the game's Apply.
//
// The game saves r_WindowType in its profile (attributes.xml, res_fs). While the borderless window
// is chosen, the profile keeps the last mode of the game's own menu instead, so the game started
// without KCDS opens as it did before; KCDS keeps the borderless window in
// %LOCALAPPDATA%\KCDS\window-mode.json and starts the game in it, with the user.cfg lines
// (branding), before the renderer opens the window.
//
// WHGame.dll 1.5.5, resolved by unique signature (display_install):
//  graphics page builder (C_UISettings, page 12): the Window mode row (button 36) with its
//                Fullscreen (value 2) and Window (value 0) choices, then the other rows, the
//                choices set from the settings in effect, and the page shown.
//  attribute writer (profile attributes, name, int value): the profile save writes res_fs, the
//                window type, through it.
#pragma once

namespace kcds::runtime::display::window_mode {

using Logger = void (*)(const char* format, ...);

// From DllMain, before branding::start: the borderless window joins the startup settings.
void start(Logger log);

struct Natives {
  void* graphics_page = nullptr;
  void* write_int_attribute = nullptr;
};

// Hooks both through the process's MinHook instance (already initialized). False, with nothing
// enabled, when one is missing or a hook fails.
bool install(const Natives& natives);

}  // namespace kcds::runtime::display::window_mode
