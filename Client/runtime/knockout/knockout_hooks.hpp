// Knock-out, faint and the photo mode: their natives found in the game and handed to knockout/
// and camera/ (split out of game_client.cpp's start).
#pragma once

#include "knockout.hpp"

namespace kcds::game_client {

// Once, as the client starts: the knock-out natives found in the game; the photo mode and the
// third-person camera installed on the game singleton they find.
kcds::runtime::knockout::Natives resolve_knockout_natives();

// Then: the knock-out, the ragdoll action and the faint gate installed. False when a native the
// client cannot run without is missing.
bool install_knockout(const kcds::runtime::knockout::Natives& knockout_natives);

}  // namespace kcds::game_client
