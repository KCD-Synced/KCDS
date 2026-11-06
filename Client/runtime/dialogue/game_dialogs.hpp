// The game's own dialogs while the local player is knocked out.
//
// Every dialog of the game starts in C_DialogManager's start ((manager, request)):
// it builds the C_DialogInstance and returns it, or null when the dialog cannot start,
// which its callers already handle. After a guard knocked a player out, the game's crime flow
// started the guard's arrest and punishment dialog (Localization/dialog/utils/punishment) with
// its cutscene on the knocked-out player's game. On a server the knockout is
// KCDS's own (knockout/), so a dialog the game starts while the local player lies knocked out is
// refused. Every start is logged with the request's kind (+ 172), to learn the other cases. A
// dialog of the local player alone (their comments on their own state) is refused while the
// monologue is disabled (hud/monologue.hpp player_alone), and every dialog while a server
// conversation (Dialogue, dialogue_list.hpp) is shown, so a nearby NPC's line does not end it.
#pragma once

namespace kcds::runtime::dialogue::game_dialogs {

using Logger = void (*)(const char* format, ...);

// Hooks the start (the address from its signature). False when it cannot.
bool install(void* start, Logger logger);

}  // namespace kcds::runtime::dialogue::game_dialogs
