// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "display_install.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "window_mode.hpp"

namespace kcds::game_client {

void install_display() {
  kcds::runtime::display::window_mode::Natives natives;
  // mov rcx,[rcx+330h]; mov dl,0Ch (page 12) into the page preparation right after the prologue.
  natives.graphics_page = resolve_unique(
      "GraphicsSettingsPage",
      "48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 68 A1 48 81 EC D8 00 00 00 48 8B F9 0F "
      "29 70 A8 48 8B 89 30 03 00 00 B2 0C");
  // The profile save writes res_x, res_y and res_fs one after the other; the call that writes
  // res_y gives the attribute writer.
  natives.write_int_attribute = resolve_unique(
      "ProfileWriteIntAttribute",
      "48 8B 8F 98 01 00 00 48 8B 01 FF 50 10 4C 8D 45 28 48 8B CB 48 8D 15 ?? ?? ?? ?? 89 45 28 "
      "E8 ?? ?? ?? ?? 4C 8D 45 B8 48 8D 15 ?? ?? ?? ?? 48 8B CB E8",
      31, 35);
  const bool installed = kcds::runtime::display::window_mode::install(natives);
  log("DISPLAY borderless window choice %s", installed ? "installed" : "unavailable");
}

}  // namespace kcds::game_client
