// The weapons a knocked-out body lets go of (the sword lay on the ground in
// every game, a copy anyone could take up, and stayed in its owner's inventory and quickslot).
//
// Every game lets a body's weapons fall as it is knocked out, each its own copy: a replica's
// game moves them into the world's inventory, and those copies stayed and piled up with every
// knock-out; the owner's game keeps them in its player's inventory while they lie, and gave them
// back at the revive. While a body stands, the items in its hands are noted; once it is down, a
// replica's that lies in the world is taken out of that game's world, and for the owner's let go
// of the hands its client asks the server to drop it, as the inventory screen's drop does
// (ground::ask_drop): the server takes it out of the player's inventory (and so out of the game's
// and its quickslot) and lays it at the body as one ground item for everyone, to take up once.
//
// Engine thread.
#ifndef KCDS_RUNTIME_KNOCKOUT_DROPPED_WEAPONS_HPP
#define KCDS_RUNTIME_KNOCKOUT_DROPPED_WEAPONS_HPP

#include <cstdint>

namespace kcds::runtime::knockout::dropped_weapons {

using Logger = void (*)(const char* format, ...);

// One body this frame: the local player's own (`own`, any key) or a replica's (its player id).
// Call for every body every frame, standing or down.
void tick(std::uint32_t key, void* actor, bool down, bool own, Logger logger);

// The body of key is gone (its replica left), or the world changed (every body).
void forget(std::uint32_t key);
void reset();

}  // namespace kcds::runtime::knockout::dropped_weapons

#endif  // KCDS_RUNTIME_KNOCKOUT_DROPPED_WEAPONS_HPP
