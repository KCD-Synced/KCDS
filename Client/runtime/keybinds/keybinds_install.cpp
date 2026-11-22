// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "keybinds_install.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "keybind_screen.hpp"

namespace kcds::game_client {

void install_keybinds() {
  kcds::runtime::keybinds::screen::Natives natives;
  natives.services = get_services;
  // C_Keybinds parse, apply of the profile's bindings and save
  // into the profile.
  natives.parse = resolve_unique("KeybindsParse",
                                 "48 89 54 24 10 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 "
                                 "57 48 8D AC 24 F8 FE FF FF");
  natives.apply = resolve_unique("KeybindsApply",
                                 "48 89 54 24 10 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 "
                                 "57 48 8D 6C 24 E1 48 81 EC 98 00 00 00");
  natives.save = resolve_unique("KeybindsSave",
                                "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 "
                                "56 41 57 48 8B EC 48 83 EC 40 4C 8B EA");
  natives.set_input = resolve_unique("KeybindsSetInput",
                                     "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 44 89 48 20 55 "
                                     "41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 60 48 8B 7D 50 4C "
                                     "8B F1 4C 63 E2");
  natives.string_from_text =
      resolve_unique("CryStringFromText",
                     "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 44 8B 05 ?? ?? "
                     "?? 04 48 8B F1 65 48 8B 04 25 58 00 00 00 48 83 CB FF B9 18 00 00 00");
  const bool installed = kcds::runtime::keybinds::screen::install(natives, log);
  log("KEYBINDS %s", installed ? "installed" : "unavailable");
}

}  // namespace kcds::game_client
