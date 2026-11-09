// Lying in a bed in a multiplayer world. The game's Sleep (E on a bed) lays the player down and its
// bed script asks whether they may sleep (C_ScriptBindPlayer CanSleepAndReportProblem); in a
// multiplayer world the time cannot be skipped, so the answer is no and the script calls OnBedStop
// (slot 24 of C_ScriptBindPlayer, which runs player:sittingAction("action(6)"), the stand-up): the
// player got up again a few seconds after lying down.
//
// Here OnBedStop is held while the local player still lies in the bed in a multiplayer world: they
// stay lying (no sleep, no time skip), and the game's own "Get up" hint on the use key (E) runs the
// held stand-up, as on a chair. Leaving the bed another way (knocked out, moved by the server,
// leaving the world) drops the hint.
//
// Engine thread.
#pragma once

namespace kcds::runtime::beds {

using Logger = void (*)(const char* format, ...);

// Hooks the game's OnBedStop; false when its function is not found.
bool install(Logger logger);

// Every frame: keeps the Get up hint while the player lies held in a bed, and drops it when they
// left it.
void frame(bool in_world);

}  // namespace kcds::runtime::beds
