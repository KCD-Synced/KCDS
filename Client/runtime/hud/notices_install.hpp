// The game's own notices used by the server: items given, speech bubbles
// levels reached, and KCDS texts under the game's localization keys.
#pragma once

namespace kcds::game_client {

// Called by initialize_adapter(): the item, bubble and level-up notices.
void install_hud_notices();
// Called by initialize_adapter(): the KCDS texts ("kcds_...") for the game's own notices.
void install_game_texts();

}  // namespace kcds::game_client
