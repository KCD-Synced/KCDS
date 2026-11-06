// The game's photo mode (F1, its free camera) allowed or not by the server (Freecam.setEnabled, a
// KCDS addition), WHGame 1.5.5.
//
// The photo mode manager (C_PhotomodeManager, its setup) listens to the action map
// "photomode_enable" (F1) and turns it on with the action map manager: the game singleton
// + 8, vtable slot 28 (+ 224) gives the IActionMapManager, whose slot 29 (+ 232)
// enables or disables an action map by name. Not allowed, that map is kept off, so F1 does
// nothing; the game may turn it on again on its own (after a menu), so it is put back every second.
//
// Leaving the photo mode, the game raises the event "HRAC_VYPNUL_PHOTOMODE" ("the player turned the
// photo mode off", called only from the photo mode's close) and the player's
// character remarks on it (dialog/open_world/photo_mode, live: tmck_hlas_henry_slusny). In a KCDS
// game that remark is skipped ("Henry says something" after F1).
//
// Engine thread.
#pragma once

namespace kcds::runtime::camera {

using PhotoLogger = void (*)(const char* format, ...);

// game: the game singleton getter; left_photo_mode: the close's remark,
// null to keep it. False when the game singleton is missing.
bool install_photo_mode(void* (*game)(), void* left_photo_mode, PhotoLogger logger);

// Whether the server lets the local player open the photo mode (allowed until it says otherwise;
// allowed again when the player leaves a world).
void set_photo_mode_allowed(bool allowed);

// Engine thread, every frame. While the local player lies knocked out the photo mode stays off
// whatever the server allows: opened then, it changed what the knocked-out game reports (its
// consciousness), every other game took the player for awake and a carry of the body was
// dropped.
void tick_photo_mode(bool in_world, double seconds, bool knocked_out);

}  // namespace kcds::runtime::camera
