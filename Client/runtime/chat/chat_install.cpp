// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "chat_install.hpp"

#include <cstdint>
#include <cstring>

#include "chat_overlay.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../input/input_gate.hpp"
#include "MinHook.h"
#include "chat_tick.hpp"

namespace kcds::game_client {

namespace native_chat = kcd2s::native_chat;

DWORD install_native_chat(const StartupTargets& targets) {
  // The target under the name initialize_adapter() gave it.
  void* const player_input_action = targets.player_input_action;
  if (void* show_console = resolve_unique("ShowConsole", kShowConsoleSignature)) {
    auto reader = static_cast<std::uint8_t*>(show_console) + 0x40;
    if (reader[0] == 0x48 && reader[1] == 0x8B && reader[2] == 0x0D) {
      std::int32_t displacement;
      std::memcpy(&displacement, reader + 3, 4);
      hardware_mouse_slot = reinterpret_cast<void**>(reader + 7 + displacement);
    }
    log("WEB hardware mouse %s", hardware_mouse_slot ? "resolved" : "unavailable");
    const bool guarded = MH_CreateHook(show_console, reinterpret_cast<void*>(hooked_show_console),
                                       reinterpret_cast<void**>(&original_show_console)) == MH_OK &&
                         MH_EnableHook(show_console) == MH_OK;
    log("CHAT console guard while typing=%u", unsigned(guarded));
  } else {
    log("CHAT console guard unavailable: ShowConsole signature not found");
  }
  if (!native_chat::install(log)) {
    log("CHAT renderer setup failed; adapter not started");
    MH_DisableHook(player_input_action);
    return 1;
  }
  // The game's swap chain present: it waits for the frame latency object when asked, then calls
  // the DXGI swap chain's Present (held at + 168) with the frame's sync and flags.
  native_chat::install_game_present(resolve_unique(
      "GameSwapChainPresent",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 33 C0 48 8D B1 A8 00 00 00 "
      "F6 81 98 00 00 00 40 41 8B F8 8B EA 48 8B D9"));
  return 0;
}

}  // namespace kcds::game_client
