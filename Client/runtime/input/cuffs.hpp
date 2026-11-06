// Handcuffs on the local player (the server's Player.setCuffed; CUFFS in
// kcd2s/integration/cuffs.hpp). While cuffed only the player's walking and looking around reach
// the game: every other action of the local player's input (attacks, blocks, drawing, using,
// jumping, crouching, sprinting, the quick slots, ...) is refused where the game takes it, the
// player input's action handler. A key's release always goes through, so nothing stays held. The
// pose is the player's animation (the server's order), not this. Off when the client leaves a
// world. Engine thread only.
#pragma once

namespace kcds::runtime::input::cuffs {

void set_cuffed(bool cuffed);
bool cuffed();

// True when the game must not get this action of the local player (`mode` 1 pressed, 2
// released, 4 held, as the action handler gets it).
bool refuse_action(const char* action, unsigned mode);

}  // namespace kcds::runtime::input::cuffs
