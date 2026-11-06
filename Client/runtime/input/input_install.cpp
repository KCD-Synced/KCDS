// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "input_install.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "MinHook.h"
#include "input_gate.hpp"
#include "input_mode.hpp"

namespace kcds::game_client {

DWORD install_interactive_state_hooks(const StartupTargets& targets) {
  // The targets under the names initialize_adapter() gave them.
  void* const interactive_state = targets.interactive_state;
  void* const move_to_start = targets.move_to_start;
  void* const move_to_end = targets.move_to_end;
  void* const enable_action_map = targets.enable_action_map;
  if (MH_CreateHook(interactive_state, reinterpret_cast<void*>(hooked_interactive_state),
                    reinterpret_cast<void**>(&original_interactive_state)) != MH_OK ||
      MH_EnableHook(interactive_state) != MH_OK)
    return 1;
  if (MH_CreateHook(move_to_start, reinterpret_cast<void*>(hooked_move_to_start),
                    reinterpret_cast<void**>(&original_move_to_start)) != MH_OK ||
      MH_EnableHook(move_to_start) != MH_OK)
    return 1;
  if (MH_CreateHook(move_to_end, reinterpret_cast<void*>(hooked_move_to_end),
                    reinterpret_cast<void**>(&original_move_to_end)) != MH_OK ||
      MH_EnableHook(move_to_end) != MH_OK)
    return 1;
  if (MH_CreateHook(enable_action_map, reinterpret_cast<void*>(hooked_enable_action_map),
                    reinterpret_cast<void**>(&original_enable_action_map)) != MH_OK ||
      MH_EnableHook(enable_action_map) != MH_OK)
    return 1;
  return 0;
}

DWORD install_player_input_hook(StartupTargets& targets) {
  auto player_input_action =
      resolve_unique("PlayerInputOnAction",
                     "48 8B C4 48 89 58 10 48 89 70 18 F3 0F 11 58 20 55 57 41 56 48 8D 68 A1 48 "
                     "81 EC 90 00 00 00 0F 29 70 D8 48 8B F9 8B 41 18 41 BE 01 00 00 00 89 41 1C");
  if (!player_input_action) {
    log("CHAT native input signature missing; adapter not started");
    return 1;
  }
  if (MH_CreateHook(player_input_action, reinterpret_cast<void*>(hooked_player_input_action),
                    reinterpret_cast<void**>(&original_player_input_action)) != MH_OK)
    return 1;
  if (MH_EnableHook(player_input_action) != MH_OK)
    return 1;
  targets.player_input_action = player_input_action;
  return 0;
}

}  // namespace kcds::game_client
