// KCDS texts under the game's own localization keys, WHGame 1.5.5.
//
// The game's notices and HUD take localization keys ("ui_hud_pickpocket_fail_lost") and look
// them up with the localization manager's LocalizeLabel (CLocalizedStringsManager, vtable slot 23,
// manager, "@key", string out, english); a key it does not know comes back as written,
// so a plain text shows every word as "@word". Keys starting with "kcds_" are answered from this
// table instead (a label override of branding's LocalizeLabel hook, the only hook on it).
//
// Any thread (the table is guarded).
#ifndef KCDS_RUNTIME_HUD_GAME_TEXTS_HPP
#define KCDS_RUNTIME_HUD_GAME_TEXTS_HPP

#include <string>

namespace kcds::runtime::hud {

// assign: the game's string assignment from text. False when it is missing or no
// label override is free.
bool install_game_texts(void* assign);

// The text the game shows for a key ("kcds_..." without the "@"); an empty text removes it.
void set_text(const std::string& key, const std::string& text);

// While set, the game's own key ("ui_SoundSettingsTitle", without the "@") reads as `text`: a page
// of ours shown in the layout of one of the game's takes its own title. An empty text gives the key
// back to the game.
void set_stand_in(const std::string& game_key, const std::string& text);

}  // namespace kcds::runtime::hud

#endif  // KCDS_RUNTIME_HUD_GAME_TEXTS_HPP
