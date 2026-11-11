#include "level_up_notice.hpp"

// The script channel before windows.h, which defines min and max macros.
#include "kcd2s/integration/script_channel.hpp"

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
// The HUD class's method that shows a stat's or skill's XP gain (its HUD, the stat, its name, the
// gain).
using ShowXpGain = Word (*)(Word, Word, Word, Word);
ShowXpGain original_show_xp_gain = nullptr;
LevelUpLogger write_log = nullptr;
std::atomic<ULONGLONG> quiet_until{0};
// Hud.setXpNotices: the game's level and XP notices, off until a resource turns them on.
std::atomic<bool> xp_notices{false};
std::atomic<bool> told_off{false};
const void* notices_session = nullptr;

// Whether the game's level or XP notice is kept off (told once in the log).
bool kept_off() {
  if (xp_notices.load()) {
    return false;
  }
  if (!told_off.exchange(true) && write_log) {
    write_log("HUD the game's level and XP notices kept off (Hud.setXpNotices)");
  }
  return true;
}

Word show_xp_gain(Word hud, Word stat, Word name, Word gain) {
  if (kept_off()) {
    return 0;
  }
  return original_show_xp_gain(hud, stat, name, gain);
}

Word show_level_up(Word hud, Word a, Word b, Word c, Word d, Word e, Word f, Word g) {
  if (kept_off()) {
    return 0;
  }
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

bool install_level_up_notice(void* level_up, void* perk_gained, void* xp_gain,
                             LevelUpLogger logger) {
  write_log = logger;
  return hook(level_up, reinterpret_cast<void*>(show_level_up),
              reinterpret_cast<void**>(&original_show_level_up)) &&
         hook(perk_gained, reinterpret_cast<void*>(show_perk_gained),
              reinterpret_cast<void**>(&original_show_perk_gained)) &&
         hook(xp_gain, reinterpret_cast<void*>(show_xp_gain),
              reinterpret_cast<void**>(&original_show_xp_gain));
}

void quiet_level_ups() {
  quiet_until.store(GetTickCount64() + kQuietMs);
}

bool handle_xp_notices_message(const kcd2s::sdk::Value& message) {
  if (kcd2s::integration::script_operation(message) != "hudXpNotices") {
    return false;
  }
  const kcd2s::sdk::Value* value = kcd2s::integration::script_field(message, "enabled");
  xp_notices.store(value && value->tag() == kcd2s::sdk::Tag::Bool && value->as_bool());
  return true;
}

void xp_notices_session(const void* session) {
  if (session != notices_session) {
    notices_session = session;
    xp_notices.store(false);
  }
}

}  // namespace kcds::runtime::hud
