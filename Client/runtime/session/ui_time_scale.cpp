#include "../core/client_internal.hpp"
// client_internal.hpp first: it brings the socket headers in before windows.h.
#include "ui_time_scale.hpp"

#include <windows.h>

#include <cstddef>

namespace kcds::runtime::session {
namespace {

// The game's slow-down ratios of its screens (ui_time_scale.hpp).
constexpr const char* kRatios[] = {"wh_ui_InventoryPauseRatio", "wh_ui_ApsePauseRatio"};
// What the world's pace is divided by: nothing.
constexpr char kNoSlowDown[] = "1";
constexpr int kNoSlowDownValue = 1;
// IConsole GetCVar (slot 23); ICVar GetIVal (slot 2) and Set(text) (slot 9), as the NPCs' observer
// setting is kept (npcs/npc_suppression.cpp).
constexpr std::size_t kConsoleGetCVar = 23;
constexpr std::size_t kCVarGetInt = 2;
constexpr std::size_t kCVarSetText = 9;
constexpr ULONGLONG kCheckMs = 3000;

Logger log_line = nullptr;
ULONGLONG next_check = 0;
bool unknown_logged = false;

}  // namespace

void set_ui_time_scale_logger(Logger logger) {
  log_line = logger;
}

void keep_ui_time_scale() {
  const ULONGLONG now = GetTickCount64();
  if (now < next_check) {
    return;
  }
  next_check = now + kCheckMs;
  void* console = kcds::game_client::console_slot ? *kcds::game_client::console_slot : nullptr;
  if (!console) {
    return;
  }
  using GetCVar = void*(__fastcall*)(void* console, const char* name);
  using GetInt = int(__fastcall*)(void* variable);
  using SetText = void(__fastcall*)(void* variable, const char* value);
  auto get = reinterpret_cast<GetCVar>((*static_cast<void***>(console))[kConsoleGetCVar]);
  for (const char* name : kRatios) {
    void* variable = get(console, name);
    if (!variable) {
      if (log_line && !unknown_logged) {
        log_line("UI TIME SCALE %s unknown", name);
      }
      unknown_logged = true;
      continue;
    }
    auto** table = *static_cast<void***>(variable);
    const int ratio = reinterpret_cast<GetInt>(table[kCVarGetInt])(variable);
    if (ratio == kNoSlowDownValue) {
      continue;
    }
    reinterpret_cast<SetText>(table[kCVarSetText])(variable, kNoSlowDown);
    if (log_line) {
      log_line("UI TIME SCALE %s was %d, now %s: the world keeps its pace in the game's screens",
               name, ratio, kNoSlowDown);
    }
  }
}

}  // namespace kcds::runtime::session
