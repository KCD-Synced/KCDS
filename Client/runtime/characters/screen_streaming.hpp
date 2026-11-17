// The characters around the player stay loaded while one of the game's screens is open.
//
// The game keeps each character's model loaded while it stands within a share of the view
// distance of the camera (wh_cc_UberlodLoadDistRatio), and lets it go once it is further than
// that and wh_cc_UnloadHysteresisDist more (WHGame 1.5.5: the state at + 0x38, 1 loaded, 0 not,
// the camera's distance at + 0xB8). An inventory, map or other screen puts the camera on its own
// view: every character around the player then counted as far, lost its skeleton within a
// tenth of a second, and the NPCs this game simulates stood frozen on every screen until a player
// in the world took them over with a brain of its own. While a screen is open, a loaded character
// stays loaded; nothing is loaded that was not.
#pragma once

namespace kcds::game_client {

// False when the game's check was not found or not hooked: the characters then go as the game
// lets them, and this game hands its NPCs over while a screen is open.
bool install_screen_streaming();

// Whether characters stay loaded while a screen is open.
bool screen_streaming_installed();

}  // namespace kcds::game_client
