// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "screen_streaming.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "MinHook.h"

namespace kcds::game_client {

namespace {

// The character's state the game's check reads and answers anew (1 loaded).
constexpr std::size_t kLoadState = 0x38;
constexpr int kLoaded = 1;

using LoadCheck = int(__fastcall*)(void* character);
LoadCheck original_check = nullptr;
std::atomic<bool> installed{false};

int __fastcall hooked_check(void* character) {
  const int answer = original_check(character);
  if (answer == kLoaded || !character || !game_menu_open()) {
    return answer;
  }
  const int state = *reinterpret_cast<const std::int32_t*>(
      static_cast<const std::uint8_t*>(character) + kLoadState);
  return state == kLoaded ? kLoaded : answer;
}

}  // namespace

bool install_screen_streaming() {
  void* check = resolve_unique("CharacterLoadCheck",
                               "40 53 48 83 EC 30 48 8B 05 ?? ?? ?? ?? 48 8B D9 0F 29 74 24 20 "
                               "48 39 41 70 74 6D 80 B9 C8 00 00 00 00 75 64 48 8B 49 10 48 8B "
                               "01 FF 90 90 04 00 00 0F 28 F0 E8 ?? ?? ?? ?? F3 0F 59 B0 7C 02 "
                               "00 00");
  if (!check) {
    log("CHARACTERS screen streaming: the load check was not found");
    return false;
  }
  const bool hooked = MH_CreateHook(check, reinterpret_cast<void*>(hooked_check),
                                    reinterpret_cast<void**>(&original_check)) == MH_OK &&
                      MH_EnableHook(check) == MH_OK;
  installed.store(hooked);
  log("CHARACTERS screen streaming %s", hooked ? "installed" : "failed");
  return hooked;
}

bool screen_streaming_installed() {
  return installed.load();
}

}  // namespace kcds::game_client
