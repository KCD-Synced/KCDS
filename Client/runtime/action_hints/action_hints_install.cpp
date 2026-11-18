// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "action_hints_install.hpp"

#include "../core/client_state.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "resource_action_hints.hpp"

namespace kcds::game_client {

void install_action_hints() {
  // ActionHint: the HUD's hint-list listener and the hint constructor.
  kcds::runtime::action_hints::Natives natives;
  natives.hud_hints_listener = resolve_unique(
      "HudHintsListener",
      "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D8 48 81 EC 28 01 00 00 48 8B D9 33 "
      "F6 48 8D 4D C8 89 B5 80 00 00 00 45 8B F8 48 8B FA E8 ?? ?? ?? ?? 48 8B 4F 08 48 8D 43 "
      "A8");
  natives.hint_constructor = resolve_unique(
      "ActionHintConstructor",
      "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 83 C0 0C 48 8D 4B 08 48 8D 15 ?? ?? ?? ?? 48 "
      "89 03 E8 ?? ?? ?? ?? C7 43 10 00 00 00 00 66 C7 43 14 01 00 C7 43 18 00 00 00 00");
  natives.hud_entity_name = resolve_unique(
      "HudEntityName",
      "48 89 5C 24 10 48 89 4C 24 08 57 48 83 EC 20 41 8B D8 48 8B FA E8 ?? ?? ?? ?? 8B D3 48 "
      "8B 88 80 01 00 00 48 8B 01 FF 50 18 48 85 C0 74 ?? 48 8B 98 68 06 00 00 48 85 DB 74 ?? "
      "48 8B 03 48 8B CB FF 90 18 02 00 00");
  natives.hud_localize = resolve_unique(
      "HudTextLocalize",
      "48 89 5C 24 18 48 89 7C 24 20 55 48 8D 6C 24 F0 48 81 EC 10 01 00 00 48 8B 05 ?? ?? ?? ?? "
      "48 33 C4 48 89 45 00 F7 05 ?? ?? ?? ?? 00 04 00 00 48 8B DA 48 8B F9 0F 85 ?? ?? ?? ?? 48 "
      "8B CA E8");
  natives.string_assign = string_assign;
  natives.menu_open = [] { return game_menu_open(); };
  const bool hints = kcds::runtime::action_hints::install(natives, log);
  log("ACTIONHINT %s", hints ? "installed" : "unavailable");
}

}  // namespace kcds::game_client
