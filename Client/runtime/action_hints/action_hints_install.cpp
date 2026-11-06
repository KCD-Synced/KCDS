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
  natives.string_assign = string_assign;
  natives.menu_open = [] { return game_menu_open(); };
  const bool hints = kcds::runtime::action_hints::install(natives, log);
  log("ACTIONHINT %s", hints ? "installed" : "unavailable");
}

}  // namespace kcds::game_client
