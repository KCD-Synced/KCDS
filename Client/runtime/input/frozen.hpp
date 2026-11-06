// A frozen local player (the server's Player.frozen; FROZEN in
// kcd2s/integration/frozen.hpp). While frozen only the player's looking around and the menus reach
// the game: walking, attacks, using, jumping and every other action of the local player's input
// are refused where the game takes them, the player input's action handler. A key's release
// always goes through, so nothing stays held. Off when the client leaves a world. Engine thread
// only.
#pragma once

namespace kcds::runtime::input::frozen {

void set_frozen(bool frozen);
bool frozen();

// True when the game must not get this action of the local player (`mode` 1 pressed, 2
// released, 4 held, as the action handler gets it).
bool refuse_action(const char* action, unsigned mode);

}  // namespace kcds::runtime::input::frozen
