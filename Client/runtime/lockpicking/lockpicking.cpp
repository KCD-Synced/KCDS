#include "lockpicking.hpp"

#include <cstddef>

#include <windows.h>

#include "MinHook.h"

namespace kcds::runtime::lockpicking {

namespace {

using Update = char(__fastcall*)(void* minigame, float seconds);
using End = void(__fastcall*)(void* minigame);

// The minigame's own end, vtable slot 4: what its fail step calls when no lockpick is
// left. It does not unlock and gives no reward, unlike.
constexpr std::size_t kEndSlot = 4;

// No update for this long: the minigame is over.
constexpr ULONGLONG kOverMs = 300;
constexpr ULONGLONG kGiveUpMs = 4000;

Update original_update = nullptr;
Logger log_line = nullptr;
void* running = nullptr;
ULONGLONG last_update = 0;
ULONGLONG ending_since = 0;

char __fastcall hooked_update(void* minigame, float seconds) {
  running = minigame;
  last_update = GetTickCount64();
  return original_update(minigame, seconds);
}

}  // namespace

bool install(const Natives& natives, Logger logger) {
  log_line = logger;
  if (!natives.update) {
    return false;
  }
  const MH_STATUS created = MH_CreateHook(natives.update, reinterpret_cast<void*>(&hooked_update),
                                          reinterpret_cast<void**>(&original_update));
  const MH_STATUS enabled = created == MH_OK ? MH_EnableHook(natives.update) : created;
  if (enabled != MH_OK && log_line) {
    log_line("LOCKPICK hook failed: %d", int(enabled));
  }
  return enabled == MH_OK;
}

bool ending_for_leave() {
  const ULONGLONG now = GetTickCount64();
  if (!running || now - last_update > kOverMs) {
    running = nullptr;
    ending_since = 0;
    return false;
  }
  if (ending_since == 0) {
    ending_since = now;
    auto table = *static_cast<void***>(running);
    reinterpret_cast<End>(table[kEndSlot])(running);
    if (log_line) {
      log_line("LOCKPICK ended for the leave");
    }
    return true;
  }
  if (now - ending_since > kGiveUpMs) {
    if (log_line) {
      log_line("LOCKPICK still running after the wait, leaving anyway");
    }
    running = nullptr;
    ending_since = 0;
    return false;
  }
  return true;
}

}  // namespace kcds::runtime::lockpicking
