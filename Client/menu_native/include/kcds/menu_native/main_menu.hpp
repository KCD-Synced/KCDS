// Native main-menu hooks of the game client. The game generates its menu buttons at
// runtime: the root (main) menu builder calls AddButton once per entry, and each AddButton pushes
// one "AddBasicButton" into the Scaleform movie. These hooks own the AddButton detour, so they can
//   - hide every native button of the main menu while our server browser is shown over it, and
//   - drop single buttons by id through a filter (the multiplayer world hides save/load).
// The pause menu has its own builder and is never affected by the main-menu switch.
//
// WHGame.dll 1.5.5, resolved by unique signature:
//  root menu builder (menu state 1): "ShowRootMenu", then the entries
//  AddButton(menu, E_ButtonId, callback, enabled, tooltip) -> "AddBasicButton"
//   AddButton consumes its by-value callback and tooltip temporaries; a dropped button releases
//   them through the game's own cleanup functions, as the original does on its no-data path.
//  first-time overlay check, run when the main menu state is entered: with
//                wh_ui_showFirstTimeOverlays set, it opens the native "first_time_overlay" with
//                the EULA page (12, wh_ui_eulaConfirmedVersion < wh_ui_eulaCurrentVersion) and/or
//                the telemetry page (13, wh_ui_telemetryConfirmed == 0) and returns true; the
//                caller then does not build the main menu. Confirming the pages
//                stores the player's answers; the main menu is built afterwards.
//                The hook only observes the result: nothing is ever accepted for the player.
//  main-menu PROS banner (state 1): fills "ProsBanners" from the publisher's
//                online banners and calls the movie's SetPros(true) for an account not linked yet;
//                a linked account gets no call; a service that is not online gets SetPros(false),
//                which shows "Deep Silver Account servers are not available..."
//                (ui_pros_banner_error). While the main-menu buttons are hidden nothing runs, as
//                for a linked account; its string helpers (+0x1C3/+0x1DC) serve the
//                other hooks.
//  menu open (all states): SetSupportUrl, SetGameMode and SetVersion with
//                wh_sys_MenuBuildInfo (via the string invoke, call at +0x1D2). In the
//                main menu (state 1) with hidden buttons the version text is set empty; our page
//                shows the version.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kcds::menu_native {

using Logger = void (*)(const char* format, ...);
// Returns the unique match of a byte signature in WHGame.dll, or null.
using Resolver = void* (*)(const char* name, const char* pattern);
// Returns true to drop the native button with this E_ButtonId.
using ButtonFilter = bool (*)(std::uint8_t id);

// Resolves and enables the hooks through the process's MinHook instance (already initialized).
// Returns false, with nothing enabled, when a signature does not match.
bool install(Resolver resolve, Logger log);

// While on, the main menu is built without its native buttons, PROS banner and version text (only
// the logo and the scene remain).
void set_hide_main_menu_buttons(bool hide);

// True once the game built its main menu: the castle scene is up and the menu can be shown.
bool main_menu_built();
// Forget that the main menu was built: the player left a world and the game builds it again.
void reset_main_menu_built();

// True from the moment the game opens its first-time overlay (EULA, telemetry) until it builds
// the main menu again. Our menu stays hidden meanwhile so the player can answer the game's pages.
bool first_time_overlay_open();

// The game's own settings page from our page's gear. The main-menu build keeps a copy of its
// Settings button's callback even while the native buttons are hidden; open_settings() asks for it
// (any thread) and frame() runs it on the game thread. False when the main menu has not been built
// yet. settings_open() stays true from the request until Back or Esc builds the main menu again,
// so our page stays hidden meanwhile.
bool open_settings();
bool settings_open();
// Serves a pending open_settings() request. Call once per game frame on the game thread.
void frame();

void set_button_filter(ButtonFilter filter);

// The pause menu's way back to the main menu in a server's world. While available() is true the
// pause menu shows an "Exit to main menu" button before "Exit game"; it opens the game's own
// confirmation, whose Yes calls leave() instead of the game's save-and-exit. Both run on the game
// thread.
struct PauseLeave {
  bool (*available)() = nullptr;
  void (*leave)() = nullptr;
};
void set_pause_leave(PauseLeave leave);

// The pause menu's "Report a bug" in a server's world. While available() is true the pause menu
// shows it before its exit buttons; a click calls open(). Both run on the game thread.
struct PauseBugReport {
  bool (*available)() = nullptr;
  void (*open)() = nullptr;
};
void set_pause_bug_report(PauseBugReport report);

// The resources' own buttons in the pause menu of a server's world: each time the
// game builds the menu they come before "Report a bug", in the places of the game's two save
// buttons (a server's world shows neither), so at most two. A click calls pressed(key) on the game
// thread.
struct MenuButton {
  std::string key;
  std::string label;
};
inline constexpr std::size_t kMaxMenuButtons = 2;
void set_menu_buttons(std::vector<MenuButton> buttons);
void set_menu_button_pressed(void (*pressed)(const std::string& key));

}  // namespace kcds::menu_native
