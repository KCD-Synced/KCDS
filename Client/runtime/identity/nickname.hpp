// The name the local player connects under: Player.nickname on
// every machine.
//
// The launcher may name the player (the KCDS_NICKNAME environment variable it starts the game
// with); otherwise it is the player's Steam name, read from the Steam API the game itself loaded
// (steam_api64.dll: SteamAPI_SteamFriends_v017, then SteamAPI_ISteamFriends_GetPersonaName).
// Without either (another store's edition, Steam not running) there is none, and the server
// calls the player "Player <id>". The server cleans the name before anyone sees it
// (kcd2s/integration/player_identity.hpp).
#pragma once

#include <string>

namespace kcds::runtime::identity {

// The local player's name as UTF-8, or "" when there is none.
std::string local_nickname();

}  // namespace kcds::runtime::identity
