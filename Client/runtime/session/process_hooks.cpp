// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"
#include "../voice/voice_client.hpp"

#include "process_hooks.hpp"

#include "../core/log.hpp"
#include "MinHook.h"

#include <unordered_set>

namespace kcds::game_client {

// The game can end its process with TerminateProcess, which skips DLL_PROCESS_DETACH, so the
// departure is announced from the process exit calls themselves. Client::disconnect() is
// idempotent, lock-free, allocation-free and non-blocking (KSTP DISCONNECT_NOTICE); the server then
// removes this player at once instead of after its inactivity timeout.
using ExitProcessFn = void(WINAPI*)(UINT);
using TerminateProcessFn = BOOL(WINAPI*)(HANDLE, UINT);
ExitProcessFn original_exit_process = nullptr;
TerminateProcessFn original_terminate_process = nullptr;
void announce_departure() {
  if (client) {
    kcds::runtime::voice::session_ended();
    client->disconnect();
  }
}
void WINAPI hooked_exit_process(UINT code) {
  announce_departure();
  original_exit_process(code);
}
BOOL WINAPI hooked_terminate_process(HANDLE process, UINT code) {
  if (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId()) {
    announce_departure();
  }
  return original_terminate_process(process, code);
}
void install_departure_hooks() {
  void* exit_target = nullptr;
  void* terminate_target = nullptr;
  bool exit_hooked =
      MH_CreateHookApiEx(L"kernel32", "ExitProcess", reinterpret_cast<void*>(hooked_exit_process),
                         reinterpret_cast<void**>(&original_exit_process), &exit_target) == MH_OK;
  bool terminate_hooked = MH_CreateHookApiEx(L"kernel32", "TerminateProcess",
                                             reinterpret_cast<void*>(hooked_terminate_process),
                                             reinterpret_cast<void**>(&original_terminate_process),
                                             &terminate_target) == MH_OK;
  exit_hooked = exit_hooked && MH_EnableHook(exit_target) == MH_OK;
  terminate_hooked = terminate_hooked && MH_EnableHook(terminate_target) == MH_OK;
  // Not fatal: without these hooks the server still removes the player after its timeout.
  if (!exit_hooked || !terminate_hooked) {
    log("DEPARTURE exit hooks unavailable exit=%u terminate=%u", unsigned(exit_hooked),
        unsigned(terminate_hooked));
    return;
  }
  log("DEPARTURE exit hooks installed");
}
// In a multiplayer world the simulation keeps running while a menu, the inventory or a focus
// change would pause a single-player game. The client of the player being hit evaluates incoming
// melee, so a paused client ignored hits while its player stood in the pause menu (observed live).
// CCryAction::PauseGame (vtable slot 13) counts pauses per reason; a refused pause
// leaves that reason's counter at zero, so the matching resume returns early in the original.
PauseGameFn original_pause_game = nullptr;
void __fastcall hooked_pause_game(void* framework, bool pause, std::uint16_t reason, bool force) {
  if (pause && multiplayer_world_ready.load()) {
    static std::unordered_set<std::uint16_t> logged_reasons;
    if (logged_reasons.insert(reason).second) {
      log("PAUSE refused in multiplayer reason=%u force=%u", unsigned(reason), unsigned(force));
    }
    return;
  }
  original_pause_game(framework, pause, reason, force);
}

}  // namespace kcds::game_client
