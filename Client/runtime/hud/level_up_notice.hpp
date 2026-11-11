// The game's notices of a level reached ("You have reached a new level ... & perk point"), of an
// XP gain (the "increased" notice with the XP bar at the top right) and of a perk gained,
// WHGame 1.5.5.
//
// The HUD shows a level with its ShowLevelUp event (C_UIHudEvents), a moment after the XP
// effect that crossed the level is applied, an XP gain with a method of its HUD class (the HUD
// movie's ShowLevelXpGain) and a perk with ShowPerkGained.
//
// - The level and XP notices are off by default on KCDS (a server decides what its players see)
//   until a resource turns them on for the session: Hud.setXpNotices. Hud.notifyXp, a resource's
//   own notice, calls the HUD movie directly and always shows.
// - The server's quiet progression orders (setLevel, addXp and loadProgression with { notify:
//   false }) hold back the level and perk notices for a short while after they are applied.
//
// Engine thread, but for the runtime message and the pump (network thread).
#ifndef KCDS_RUNTIME_HUD_LEVEL_UP_NOTICE_HPP
#define KCDS_RUNTIME_HUD_LEVEL_UP_NOTICE_HPP

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::hud {

using LevelUpLogger = void (*)(const char* format, ...);

// The HUD's ShowLevelUp, ShowPerkGained and XP gain. False when one is missing or a hook fails.
bool install_level_up_notice(void* show_level_up, void* show_perk_gained, void* show_xp_gain,
                             LevelUpLogger logger);

// A quiet order was applied: the level-ups and perks it brings are not announced.
void quiet_level_ups();

// { op: "hudXpNotices", enabled }; true when the message was this module's.
bool handle_xp_notices_message(const kcd2s::sdk::Value& message);

// A new session turns the game's level and XP notices off again.
void xp_notices_session(const void* session);

}  // namespace kcds::runtime::hud

#endif  // KCDS_RUNTIME_HUD_LEVEL_UP_NOTICE_HPP
