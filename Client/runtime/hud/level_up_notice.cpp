#include "level_up_notice.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>

#include "MinHook.h"

namespace kcds::runtime::hud {
namespace {

// How long after a quiet order its level-ups are held back: the XP effect is applied with the
// soul's next update and the HUD announces the level right after.
constexpr ULONGLONG kQuietMs = 2000;

// The HUD's ShowLevelUp (C_UIHudEvents: the HUD, then the event's type, name, level, perk points
// and id; the last ones on the stack). Passed through untouched; its result is not used.
using Word = std::uint64_t;
using ShowLevelUp = Word (*)(Word, Word, Word, Word, Word, Word, Word, Word);

ShowLevelUp original_show_level_up = nullptr;
// The HUD's ShowPerkGained (C_UIHudEvents: the HUD's events, the event, the perk's name).
using ShowPerkGained = Word (*)(Word, Word, Word);
ShowPerkGained original_show_perk_gained = nullptr;
LevelUpLogger write_log = nullptr;
std::atomic<ULONGLONG> quiet_until{0};

Word show_level_up(Word hud, Word a, Word b, Word c, Word d, Word e, Word f, Word g) {
  if (GetTickCount64() < quiet_until.load()) {
    if (write_log) {
      write_log("PROGRESSION level-up notice held back (quiet order)");
    }
    return 0;
  }
  return original_show_level_up(hud, a, b, c, d, e, f, g);
}

Word show_perk_gained(Word events, Word event, Word perk) {
  if (GetTickCount64() < quiet_until.load()) {
    if (write_log) {
      write_log("PROGRESSION perk notice held back (quiet order)");
    }
    return 0;
  }
  return original_show_perk_gained(events, event, perk);
}

bool hook(void* target, void* detour, void** original) {
  return target && MH_CreateHook(target, detour, original) == MH_OK &&
         MH_EnableHook(target) == MH_OK;
}

}  // namespace

bool install_level_up_notice(void* level_up, void* perk_gained, LevelUpLogger logger) {
  write_log = logger;
  return hook(level_up, reinterpret_cast<void*>(show_level_up),
              reinterpret_cast<void**>(&original_show_level_up)) &&
         hook(perk_gained, reinterpret_cast<void*>(show_perk_gained),
              reinterpret_cast<void**>(&original_show_perk_gained));
}

void quiet_level_ups() {
  quiet_until.store(GetTickCount64() + kQuietMs);
}

}  // namespace kcds::runtime::hud
