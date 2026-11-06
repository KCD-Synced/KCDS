// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "single_player_screens_install.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "MinHook.h"
#include "single_player_screens.hpp"

namespace kcds::game_client {

DWORD install_single_player_screen_hooks() {
  auto skip_time = resolve_unique(
      "StartSkipTime",
      "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 48 81 EC 10 01 00 00 0F 29 70 E8 8B DA 0F "
      "29 78 D8 0F 28 F3 0F 28 FA 48 8B F1 E8 ?? ?? ?? ?? 48 8B E8 48 85 C0");
  auto tutorials = resolve_tutorial_query();
  if (!skip_time || !tutorials) {
    log("Multiplayer menu signatures missing; no game startup");
    return 1;
  }
  if (MH_CreateHook(skip_time, reinterpret_cast<void*>(hooked_start_skip_time),
                    reinterpret_cast<void**>(&original_start_skip_time)) != MH_OK ||
      MH_EnableHook(skip_time) != MH_OK)
    return 1;
  if (MH_CreateHook(tutorials, reinterpret_cast<void*>(hooked_tutorials_enabled),
                    reinterpret_cast<void**>(&original_tutorials_enabled)) != MH_OK ||
      MH_EnableHook(tutorials) != MH_OK)
    return 1;
  return 0;
}

}  // namespace kcds::game_client
