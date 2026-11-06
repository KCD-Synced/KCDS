// The players a server allows in the game's private and prohibited areas
// (Player.setTrespassAllowed, Interactions 'trespass'). The game decides how an NPC
// reacts to someone in such an area in one node, which first asks whether the context
// crime_disabledTrespassReactions is set ("turns the reactions to trespass off everywhere in the
// world"). On a client the only trespasser its NPCs react to is the local player, so
// that answer is this player's.
#pragma once

namespace kcds::runtime::world::trespass {

using Logger = void (*)(const char* format, ...);

// Hooks the game's query. False when it was not found: the NPCs keep their reactions.
bool install(void* reactions_disabled, Logger log);

// The server's rule for the local player (any thread). A client leaving a world goes back to false.
void set_allowed(bool allowed);

}  // namespace kcds::runtime::world::trespass
