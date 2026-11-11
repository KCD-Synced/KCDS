// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "session_guards_install.hpp"

#include <cstdint>
#include <cstring>

#include "../core/client_state.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../dialogue/dialog_twin_guard.hpp"
#include "../dialogue/game_dialogs.hpp"
#include "../hud/character_stats/character_stats.hpp"
#include "../hud/monologue.hpp"
#include "../hud/story_cutscenes.hpp"
#include "../replicas/replica_removal.hpp"
#include "MinHook.h"
#include "process_hooks.hpp"
#include "ui_time_scale.hpp"

namespace kcds::game_client {

void install_session_guards() {
  // The world's pace in the game's screens (ui_time_scale.hpp) logs what it changes.
  kcds::runtime::session::set_ui_time_scale_logger(log);
  // Story cutscenes a quest plays (Hud.setCutscenes): the cutscene
  // player's queue of the quest node's cutscene, and its listeners' notification it calls last
  // (the E8 at + 68).
  {
    constexpr char kCutsceneQueue[] =
        "48 89 5C 24 08 57 48 83 EC 20 48 8B 02 48 8B DA 48 8B F9 48 83 78 20 00 74 2F 48 83 C1 48 "
        "E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B D7 48 8B 88 E8 00 00 00 48 8B 49 70 E8 ?? ?? ?? ?? 45 "
        "33 C0 48 8B D3 48 8B CF E8";
    kcds::runtime::hud::story_cutscenes::Natives cutscenes;
    cutscenes.queue = resolve_unique("CutscenePlayerQueue", kCutsceneQueue);
    cutscenes.notify = resolve_unique("CutsceneNotify", kCutsceneQueue, 69, 73);
    const bool kept = kcds::runtime::hud::story_cutscenes::install(cutscenes, log);
    log("HUD story cutscenes %s", kept ? "suppressed" : "unavailable");
  }
  // The inventory's character panel (Hud.setCharacterStats): the hand-over of a row list as a
  // screen's array.
  {
    kcds::runtime::hud::character_stats::Natives stats;
    stats.set_array = resolve_unique(
        "UIFlashObjectArraySet",
        "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 48 8B F1 49 8B D8 48 8D 4C 24 20 48 8B FA "
        "E8 ?? ?? ?? ?? 4C 8D 44 24 20 48");
    stats.call_screen = resolve_unique(
        "UIFlashObjectCall",
        "48 89 5C 24 08 57 48 83 EC 60 48 8B 01 48 8B FA 48 8B D9 FF 50 60 48 8B 5B 48 48 85 DB "
        "74 43 48 8D 4C 24 30 E8");
    stats.string_assign = string_assign;
    const bool hooked = kcds::runtime::hud::character_stats::install(stats, log);
    log("HUD character stats %s", hooked ? "hooked" : "unavailable");
  }
  // The game's own dialogs, refused while the local player lies knocked out: the
  // dialog manager's start.
  {
    constexpr char kDialogStart[] =
        "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 50 83 79 78 00 48 8B FA 48 "
        "8B D9 0F 84 ?? ?? ?? ?? 48 8D 4A 58 E8";
    const bool kept = kcds::runtime::dialogue::game_dialogs::install(
        resolve_unique("DialogManagerStart", kDialogStart), log);
    log("DIALOG game dialogs %s", kept ? "watched" : "unavailable");
  }
  // The game's set-up of a dialogue stand-in, guarded against a stand-in it could not make.
  {
    const bool guarded = kcds::runtime::dialogue::twin_guard::install(
        resolve_unique("DialogTwinSetUp",
                       "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F9 49 8B F0 48 8B CA "
                       "48 8B DA E8 ?? ?? ?? ?? 84 C0 75 33 48 8B 03 BA 0A 00 00 00 48 8B CB FF"),
        log);
    log("DIALOG stand-ins %s", guarded ? "guarded" : "unguarded");
  }
  // Optional: without it, restore_local_ui() still rebinds after a remote removal.
  auto client_player_unbind = resolve_unique(
      "ClientPlayerUnbind",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 91 90 00 00 00 48 8B D9 48 8B 49 20 E8");
  if (client_player_unbind &&
      MH_CreateHook(client_player_unbind, reinterpret_cast<void*>(hooked_client_player_unbind),
                    reinterpret_cast<void**>(&original_client_player_unbind)) == MH_OK &&
      MH_EnableHook(client_player_unbind) == MH_OK) {
    log("UI PLAYER unbind guard installed");
  } else {
    log("UI PLAYER unbind guard unavailable");
  }
  // Optional: without it a player in a menu pauses the local simulation as before.
  auto pause_game = resolve_unique(
      "PauseGame",
      "48 89 5C 24 08 48 89 74 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 81 EC 80 00 "
      "00 00 45 33 E4 45 0F B7 F8 41 8A F1 44 8A F2 48 8B D9 84 D2 0F 84");
  if (pause_game &&
      MH_CreateHook(pause_game, reinterpret_cast<void*>(hooked_pause_game),
                    reinterpret_cast<void**>(&original_pause_game)) == MH_OK &&
      MH_EnableHook(pause_game) == MH_OK) {
    log("PAUSE multiplayer guard installed");
  } else {
    log("PAUSE multiplayer guard unavailable");
  }
  // Optional: without it, Quit in the server browser falls back to closing the window.
  if (auto exit_game = static_cast<std::uint8_t*>(resolve_unique(
          "NativeExitGame",
          "48 89 4C 24 08 48 83 EC 38 48 8B 0D ?? ?? ?? ?? 4C 8D 44 24 40 48 8D 15 ?? ?? ?? ?? 48 "
          "8B 01 FF 50 38 E8 ?? ?? ?? ?? 41 B0 01 48 8D 54 24 40 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 45 "
          "33 C0 48 C7 44 24 28 2A 00 00 00 48 8D 54 24 20 48 8B 88 70 01 00 00"))) {
    // The env getter is the call at +0x34 (E8 rel32).
    std::int32_t relative = 0;
    std::memcpy(&relative, exit_game + 0x35, sizeof(relative));
    native_env = reinterpret_cast<NativeEnvGetter>(exit_game + 0x34 + 5 + relative);
    log("MENU native exit resolved env getter=%p", reinterpret_cast<void*>(native_env));
    // The local character's monologue (Hud.setInnerVoice).
    kcds::runtime::hud::monologue::Natives monologue;
    monologue.env = reinterpret_cast<void*>(native_env);
    monologue.send_signal = resolve_unique(
        "AiSignalToEntity",
        "48 8B C4 48 89 58 10 48 89 68 18 48 89 70 20 48 89 48 08 57 41 56 41 57 48 83 EC 50 8B "
        "CA 4D 8B");
    const bool silenced = kcds::runtime::hud::monologue::install(monologue, log);
    log("HUD monologue %s", silenced ? "installed" : "unavailable");
  } else {
    log("MENU native exit signature missing");
  }
}

}  // namespace kcds::game_client
