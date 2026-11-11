// The world keeps its pace while the player is in one of the game's screens. A single-player game
// slows its time while the inventory or another full screen is open: the time scale is divided by
// wh_ui_InventoryPauseRatio in the inventory and by wh_ui_ApsePauseRatio in the other full screens
// (the game's own descriptions of them). In a multiplayer world that froze every NPC and animal
// this game simulates, for every player, while its player looked at their bag. Both ratios are
// kept at 1: read every kCheckMs, set again when the game set its own (a level load does).
//
// Engine thread.
#pragma once

namespace kcds::runtime::session {

using Logger = void (*)(const char* format, ...);

void set_ui_time_scale_logger(Logger logger);

// Every frame in a world.
void keep_ui_time_scale();

}  // namespace kcds::runtime::session
