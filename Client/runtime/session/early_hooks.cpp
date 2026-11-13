// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "early_hooks.hpp"

#include "../architect/local_mode.hpp"
#include "../assets/memory_paks.hpp"
#include "../branding/loading_screen.hpp"
#include "../branding/loading_tail.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../discord/discord_presence.hpp"
#include "../pose_animation/joint_turns.hpp"
#include "../studio/studio_scene.hpp"
#include "../game_mods/game_mods.hpp"
#include "../world/single_player_screens.hpp"
#include "MinHook.h"
#include "kcds/menu_native/main_menu.hpp"
#include "network_session.hpp"
#include "profile_settings.hpp"
#include "server_address.hpp"
#include "server_browser.hpp"

namespace kcds::game_client {

void install_early_hooks(StartupTargets& targets) {
  // First hook, installed right after WHGame.dll is mapped: the game logs in its user profile a few
  // seconds later, and the remaining signature scans take longer than that. Optional: without it
  // the world starts without waiting for the profile (see frame()).
  const auto early_hooks = MH_Initialize();
  auto load_profile_settings = resolve_unique(
      "LoadProfileSettings",
      "48 85 D2 0F 84 ?? ?? ?? ?? 48 8B C4 48 89 58 18 48 89 70 20 55 57 41 56 48 8D 68 98 48 81 "
      "EC 50 01 00 00 0F 29 70 D8 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 30 48 8B DA C7 44 24 20 "
      "00 00 00 00 48 8B F1");
  if ((early_hooks == MH_OK || early_hooks == MH_ERROR_ALREADY_INITIALIZED) &&
      load_profile_settings &&
      MH_CreateHook(load_profile_settings, reinterpret_cast<void*>(hooked_load_profile_settings),
                    reinterpret_cast<void**>(&original_load_profile_settings)) == MH_OK &&
      MH_EnableHook(load_profile_settings) == MH_OK) {
    log("PROFILE settings hook installed");
  } else {
    original_load_profile_settings = nullptr;
    log("PROFILE settings hook unavailable");
  }
  // Paks KCDS hands the game from memory, before the game opens any level.
  if (early_hooks == MH_OK || early_hooks == MH_ERROR_ALREADY_INITIALIZED) {
    kcds::runtime::assets::memory_paks::install(
        [](const char* name, const char* pattern, int displacement, int length) {
          return resolve_unique(name, pattern, displacement, length);
        },
        log);
    // The studio's level is listed only when its stub is on disk as the game starts.
    kcds::runtime::studio::prepare_level_listing();
    if (kcds::runtime::architect::local_mode::enabled() && kcds::runtime::studio::requested()) {
      kcds::runtime::studio::provide_level();
    }
    // Joint turns on characters: the studio's posing and the players' pose
    // animations, through the game's joint operator queue.
    kcds::runtime::pose_animation::joint_turns::install(
        [](const char* name, const char* pattern, int displacement, int length) {
          return resolve_unique(name, pattern, displacement, length);
        },
        log);
  }
  // The player's own game mods stay off while playing KCDS; the game finds them
  // soon after WHGame.dll loads.
  if (early_hooks == MH_OK || early_hooks == MH_ERROR_ALREADY_INITIALIZED) {
    kcds::runtime::game_mods::install(
        [](const char* name, const char* pattern) { return resolve_unique(name, pattern); }, log);
  }
  // Main-menu hooks as early as the profile hook: the menu is built soon after WHGame.dll loads.
  const bool menu_hooks_installed =
      (early_hooks == MH_OK || early_hooks == MH_ERROR_ALREADY_INITIALIZED) &&
      kcds::menu_native::install(
          [](const char* name, const char* pattern) { return resolve_unique(name, pattern); }, log);
  kcds::menu_native::set_button_filter(multiplayer_button_filter);
  // Our menu shows whenever it can, also when the launcher names the server: the page joins that
  // server itself, so the player sees KCDS's connecting screen and, after leaving, the server
  // list instead of the game's single-player menu.
  // World Architect's local mode shows no server list and joins no server.
  browser_mode = menu_hooks_installed && !kcds::runtime::architect::local_mode::enabled();
  // The player's Discord activity: "Starting up" until the main menu shows.
  kcds::runtime::discord::start(KCDS_GAME_CLIENT_VERSION, log);
  if (browser_mode) {
    // ESC menu: leave the server's world back to the server list (no save, no restart).
    kcds::menu_native::PauseLeave pause_leave;
    pause_leave.available = [] { return multiplayer_world.load() && !leave_requested.load(); };
    pause_leave.leave = [] {
      log("MENU leave requested from the pause menu");
      leave_requested.store(true);
    };
    kcds::menu_native::set_pause_leave(pause_leave);
    // ESC menu: report a bug in KCDS to the team, with the player's logs if they agree.
    kcds::menu_native::PauseBugReport pause_bug_report;
    pause_bug_report.available = [] { return multiplayer_world.load() && !leave_requested.load(); };
    pause_bug_report.open = [] { request_bug_report(); };
    kcds::menu_native::set_pause_bug_report(pause_bug_report);
  }
  // KCDS's loading screen until the game shows its main menu or a first-start page. The
  // frame hooks that paint it come with the server browser, right after this. World Architect's
  // local mode shows no menu (its buttons hidden, only its zoomed background art for seconds
  // while the world waits for the player's profile: owner, "fix that loading"): there
  // it stays until the world is loading and the game's own loading screen is drawn whole (its
  // first seconds are a frozen, zoomed frame of its picture).
  if (kcds::runtime::architect::local_mode::enabled()) {
    kcds::runtime::branding::loading_screen::set_status(L"Loading Editor");
  }
  kcds::runtime::branding::loading_screen::install(
      [] {
        if (kcds::menu_native::first_time_overlay_open()) {
          return true;
        }
        if (kcds::runtime::architect::local_mode::enabled()) {
          // Until the editor is drawn, as for a tool started from the main menu: the world and
          // its HUD showed for a second or more before it.
          return world_requested.load() &&
                 kcds::runtime::branding::loading_tail::level_screen_shown() &&
                 kcds::runtime::architect::local_mode::editor_shown();
        }
        // Without the menu hooks the menu can't be seen being built: no screen held to its limit.
        return kcds::menu_native::main_menu_built() || kcds::menu_native::hooks_unavailable();
      },
      log);
  quit_on_window_close();
  if (browser_mode) {
    start_browser();
  }
  targets.menu_hooks_installed = menu_hooks_installed;
}

}  // namespace kcds::game_client
