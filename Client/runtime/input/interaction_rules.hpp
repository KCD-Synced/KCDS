// The game's own interactions the server allows the local player (the server's Interactions.set
// and Player.setInteraction; INTERACTION_RULES in kcd2s/integration/interaction_rules.hpp). On a
// KCDS server they start off: the companion key's whistle is refused where the game takes the
// action (the player input's action handler), and the NPC and player prompts ask allowed().
// Nothing is allowed again when the client leaves a world. Engine thread only.
#pragma once

#include <cstdint>

namespace kcds::runtime::input::interaction_rules {

// The server's bits (kAllow* of interaction_rules.hpp). The NPCs' own talk follows its bit
// through the game's dialog switch wh_dlg_Enable (console: the game's IConsole). Out of a world
// (set_allowed(0) on leaving) the game's talk comes back: pass in_world false.
void set_allowed(std::uint16_t bits, void* console = nullptr, bool in_world = true);
bool allowed(std::uint16_t bit);

// True when the game must not get this action of the local player (`mode` 1 pressed, 2
// released, 4 held, as the action handler gets it): the whistle while it is not allowed.
bool refuse_action(const char* action, unsigned mode);

}  // namespace kcds::runtime::input::interaction_rules
