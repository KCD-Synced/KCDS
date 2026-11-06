// The sleep and food needs turned off by the server (Needs.setEnabled), WHGame 1.5.5.
//
// The soul's timer set runs, among others, two need timers while the soul's need
// multiplier (soul + 900, the soul table's digestion_multiplier: 1 for the player, 0 for NPCs) is
// not 0:
//
//   food: lowers the hunger state (3) by its rate, the RPG parameter + 0x3DC times the
//               game time ratio (+ 0x3A8) and the multiplier, through the soul's modifier "dig"
//   sleep: lowers the exhaust state (2) by its rate, the RPG parameter
//               ExhaustionSpeed (+ 0x4F4) times the time ratio and the multiplier, through "exh"
//
// Both return at once while the game's own development switch wh_rpg_SkipFoodTimers ("disable all
// food-related timers (like starvation)", the RPG module's settings + 0x58, an int cvar) is not 0;
// no time is banked, so the states run on from where they are once it is 0 again. While the server
// has the needs off, the client sets that cvar to 1 through the console (IConsole::GetCVar, slot
// 23; ICVar::Set(int), slot 7, as the client sets wh_ui_DisableNewGameDebugFader), once per
// change, and back to 0 when they are on again or the player leaves the world. The cvar applies to
// every soul of this game; the replicas' states follow their owners anyway (stats::
// keep_replica_vitals) and NPC souls have no need timers.
//
// Engine thread.
#pragma once

namespace kcds::runtime::needs {

using Logger = void (*)(const char* format, ...);

// console_slot: where the game keeps its IConsole (gEnv->pConsole), read at each change.
void install(void** console_slot, Logger logger);

// Whether the server lets the local player's needs run (true until it says otherwise; true again
// when the player leaves a world). body_awake: whether the local player's body is awake now. The
// server fills sleep and food when it turns the needs off, but neither a knocked-out body (the
// server does not send it) nor a fainted one (the client drops stat orders to a body that is not
// awake) takes that fill; such a player would wake at the same low value, which can no longer
// change, and the game's exhaustion would faint them again and again. Their fill is kept for
// when the body is awake again.
void set_enabled(bool enabled, bool body_awake);

// Engine thread, every frame. Out of a world the needs run again; the next server says when they
// are off. True once when a kept fill is due: in a world, the needs still off and the body awake;
// the caller then fills exhaust and hunger through the game's own state setter.
bool tick(bool in_world, bool body_awake);

}  // namespace kcds::runtime::needs
