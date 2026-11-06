// The game's notices of a level reached ("You have reached a new level ... & perk point") and of a
// perk gained, held back for the server's quiet progression orders (setLevel, addXp and
// loadProgression with { notify: false }), WHGame 1.5.5.
//
// The HUD shows it with its ShowLevelUp event (C_UIHudEvents), a moment after the XP
// effect that crossed the level is applied, and a perk with ShowPerkGained, so a quiet
// order holds both back for a short while after it is applied.
//
// Engine thread.
#ifndef KCDS_RUNTIME_HUD_LEVEL_UP_NOTICE_HPP
#define KCDS_RUNTIME_HUD_LEVEL_UP_NOTICE_HPP

namespace kcds::runtime::hud {

using LevelUpLogger = void (*)(const char* format, ...);

// The HUD's ShowLevelUp and ShowPerkGained. False when one is missing or
// a hook fails.
bool install_level_up_notice(void* show_level_up, void* show_perk_gained, LevelUpLogger logger);

// A quiet order was applied: the level-ups and perks it brings are not announced.
void quiet_level_ups();

}  // namespace kcds::runtime::hud

#endif  // KCDS_RUNTIME_HUD_LEVEL_UP_NOTICE_HPP
