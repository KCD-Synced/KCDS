// The level's graves, kept in step with the server, WHGame 1.5.5.
//
// A grave is an entity of the script class "Hole" (Scripts/Entities/WH/Others/Hole.lua): its
// script's nDepth is how far it is dug, 0 full to 1 dug out. The game's digging minigame
// (C_HoleDigging) writes nDepth when the digging stops (1 once the digging time is
// reached, else the share dug) and hides the entity, the soil over the pit, once it reaches 1; a
// full grave is shown again (the game's own state handler, and its reset).
//
//   report   The graves of the level are read twice a second (the entity system's iterator by
//            class): a grave seen for the first time is reported (GRAVE kReport) with the state
//            the game shows.
//   dig      A grave that reached nDepth 1 since the last read without the server setting it so is
//            the local player's digging: it is told as GRAVE kDug.
//   apply    A GRAVE kSet puts the grave into the server's state the way the game's handler does:
//            nDepth 1 and the entity hidden, or nDepth 0, usable again and shown. A grave the
//            game has not streamed in yet is set when it is.
//   digging  The minigame opens the soil (hides the entity) while it digs: a grave hidden while
//            not dug out is the local player digging it, told as GRAVE kDigging (1), and a grave
//            shown again before it was dug out (the player stopped) as kDigging (0). Another
//            player's kDigging hides or shows this game's soil of that grave the same way.
//
// Engine thread.
#pragma once

#include <vector>

#include "kcd2s/integration/graves.hpp"

namespace kcds::runtime::graves {

// Every frame in a ready world with the server's grave states since the last frame, oldest
// first. Returns the GRAVE messages to send.
std::vector<kcd2s::integration::Grave> frame(double now,
                                             const std::vector<kcd2s::integration::Grave>& sets);

// A world is left: what was read and set is forgotten.
void reset();

}  // namespace kcds::runtime::graves
