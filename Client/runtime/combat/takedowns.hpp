// The game's takedowns (knock out, stealth kill, mercy kill, the pull from a horse: the actions on
// the same key) refused on the bodies KCDS moves, WHGame 1.5.5.
//
// A takedown starts in (its object: the attacker's actor at + 8, the target's entity id
// at + 120). Once the pair is set up it raises its start on the target's own takedown listeners
// (the target's + 1016 object, + 48), and one of them reads an object our bodies do not have:
// knocking out a server NPC in a fight crashed the attacker's game (a null object).
// A takedown whose target is another player's body or a server NPC's is therefore refused at its
// start, as the game refuses one whose conditions fail; the game's own NPCs keep theirs.
//
// Engine thread.
#pragma once

namespace kcds::game_client {

// Installs the guard at the takedown start (found by its bytes); false when it could not.
bool install_takedown_guard();

}  // namespace kcds::game_client
