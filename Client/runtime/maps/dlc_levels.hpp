// KCD2's levels that come with a DLC (klaster: Mysteria Ecclesiae). The game ships their files to
// every copy, so having a level's files says nothing about the DLC; the game's Steam session says
// whether the account has it. The server decides by the player's licence pass (the Master reads
// Steam's signed ticket); this keeps the game itself from loading a DLC's level it lacks.
#pragma once

#include <cstdint>
#include <string>

namespace kcds::runtime::maps::dlc {

// The Steam app id of the DLC a level comes with; 0 for a level of the base game.
std::uint32_t app_of(const std::string& level);

// False when the level comes with a DLC that the game's Steam session says this account lacks;
// true otherwise (a base game level, or a game without Steam, where the server's check decides).
bool owned(const std::string& level);

}  // namespace kcds::runtime::maps::dlc
