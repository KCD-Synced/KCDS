// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "notices_install.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "game_texts.hpp"
#include "item_notice.hpp"
#include "level_up_notice.hpp"
#include "speech_bubbles.hpp"

namespace kcds::game_client {

void install_hud_notices() {
  // The game's notice of items the server gave.
  log("ITEM notices %s", kcds::runtime::hud::install_item_notice(resolve_unique(
                             "GameServices",
                             "40 53 48 83 EC 20 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA 18 "
                             "00 00 00 48 8B 04 C8 8B 04 02 39 05 ?? ?? ?? ?? 7F ?? 48 8B 05 ?? ?? "
                             "?? ?? 48 83 C4 20"))
                             ? "installed"
                             : "unavailable");
  // The game's speech bubbles over a head, for what an NPC says (Npc.say).
  {
    kcds::runtime::hud::BubbleNatives bubbles;
    bubbles.services = get_services;
    bubbles.string_from_text =
        resolve_unique("CryStringFromText",
                       "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 44 8B 05 ?? ?? "
                       "?? 04 48 8B F1 65 48 8B 04 25 58 00 00 00 48 83 CB FF B9 18 00 00 00");
    bubbles.string_free =
        resolve_unique("CryStringFree", "48 83 EC 28 48 8B 09 48 83 C1 F4 E8 ?? ?? ?? ??");
    // SetText of the game's subtitles, found by its code.
    bubbles.subtitle_set_text = resolve_unique(
        "SubtitleSetText",
        "48 89 5C 24 10 48 89 74 24 18 44 88 4C 24 20 55 57 41 56 48 8B EC 48 83 EC 50 C7 45 20 00 "
        "00 00 00 48 8D 71 A8 48 8B 4E 70 4D 8B F0 48 8B DA 48 8B 01 FF 50 10 85 C0");
    // C_UISubtitles' constructor: its vtables written, the interface at + 0x58.
    // Other GUI modules' constructors are the same code with their own vtables: the RTTI class of
    // the vtable stored at the object's start (the lea at + 20) picks this one.
    bubbles.subtitles_constructor = resolve_by_vtable_class(
        "SubtitlesConstructor",
        "48 83 EC 28 E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 41 10 4C 8D 05 ?? ?? ?? ?? 48 8D 05 "
        "?? ?? ?? ?? 4C 89 01 48 89 41 58",
        23, 27, ".?AVC_UISubtitles@guimodule@wh@@", false);
    bubbles.log = log;
    log("SUBTITLES SetText %p, module constructor %p", bubbles.subtitle_set_text,
        bubbles.subtitles_constructor);
    kcds::runtime::hud::install_bubbles(bubbles);
    const bool installed = bubbles.services && bubbles.string_from_text && bubbles.string_free;
    log("SPEECH bubbles %s", installed ? "installed" : "unavailable");
  }
  // The game's notices of a level reached and of an XP gain (off until Hud.setXpNotices turns
  // them on), and of a perk gained; level and perk held back for quiet progression orders.
  log("LEVEL-UP notices %s",
      kcds::runtime::hud::install_level_up_notice(
          resolve_unique("HudShowLevelUp",
                         "40 55 53 56 57 41 56 48 8B EC 48 83 EC 70 49 8B D8 44 89 4D D0 4C 8B "
                         "F2 48 8B F1 E8 ?? ?? ?? ?? 48 8D 55 F0 48 C7 45 F8 00 00 00 00 48 8B "
                         "88 D8"),
          resolve_unique("HudShowPerkGained",
                         "48 89 5C 24 10 55 56 57 48 83 EC 30 F6 05 ?? ?? ?? ?? 40 48 8B F2 49 8B "
                         "D0 48 8B E9 74 13"),
          resolve_unique("HudShowLevelXpGain",
                         "48 89 5C 24 10 48 89 74 24 18 44 89 4C 24 20 55 57 41 56 48 8B EC 48 "
                         "83 EC 40 F6 05 ?? ?? ?? ?? 40 48 8B F2 49 8B D0 4C 8B F1 74 12 48 8D "
                         "4D F8"),
          log)
          ? "installed"
          : "unavailable");
}

void install_game_texts() {
  // KCDS texts under the game's localization keys ("kcds_..."), for the game's own notices.
  log("GAME TEXTS %s", kcds::runtime::hud::install_game_texts(resolve_unique(
                           "CryStringAssign",
                           "40 53 48 83 EC 20 48 8B D9 4C 8B CA 48 8B CA E8 18 00 00 00 4C 8B C0 "
                           "49 8B D1 48 8B CB E8"))
                           ? "installed"
                           : "unavailable");
}

}  // namespace kcds::game_client
