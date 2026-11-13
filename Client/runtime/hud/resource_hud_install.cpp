// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "resource_hud_install.hpp"

#include <cstdint>
#include <cstring>

#include "../core/client_state.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../world/single_player_screens.hpp"
#include "resource_hud.hpp"

namespace kcds::game_client {

void install_resource_hud() {
  // The resource Hud through the game's natives.
  kcds::runtime::hud::Natives natives;
  natives.services = get_services;
  natives.string_assign = string_assign;
  natives.string_init =
      resolve_unique("CryStringInitEmpty",
                     "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 83 C0 0C 48 89 03 48 8B C3 48 "
                     "83 C4 20 5B C3 CC CC 48 89 5C 24 08 48 89 74 24 10 48 89");
  natives.string_free =
      resolve_unique("CryStringFree", "48 83 EC 28 48 8B 09 48 83 C1 F4 E8 ?? ?? ?? ??");
  natives.tutorial_show = resolve_unique("TutorialShow", "40 53 48 83 EC 60 48 8B D9 4C 8B C2");
  natives.tutorial_reset =
      resolve_unique("TutorialReset",
                     "40 53 48 83 EC 30 48 8B D9 4C 8B C2 48 8D 54 24 20 48 8D 0D ?? ?? ?? ?? "
                     "E8 ?? ?? ?? ?? 48 8B 54 24 20");
  natives.tutorial_hide =
      resolve_unique("TutorialHide", "40 53 48 83 EC 30 48 8B DA 48 8D 0D ?? ?? ?? ??");
  // `mov rcx, [rip + disp32]` of the 3D engine before SetPostEffectParam in the script
  // binding's SetScreenFx.
  if (auto load = static_cast<std::uint8_t*>(
          resolve_unique("PostEffectEngine",
                         "48 8B 0D ?? ?? ?? ?? 45 33 C9 F3 0F 10 55 18 48 8B 55 20 48 8B 01 "
                         "FF 90 58 05 00 00"))) {
    std::int32_t displacement;
    std::memcpy(&displacement, load + 3, 4);
    natives.engine_3d = reinterpret_cast<void* const*>(load + 7 + displacement);
    engine_3d_slot = natives.engine_3d;
  }
  natives.allow_tutorials = [](bool allow) { resource_tutorial_call = allow; };
  hud_natives_found = kcds::runtime::hud::install(natives);
  log("HUD natives %s", hud_natives_found.load() ? "found" : "unavailable");
}

}  // namespace kcds::game_client
