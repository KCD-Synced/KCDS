// Items the local player wears for a while outside their inventory (Player.wearItem):
// a noose, a sack, shackles. The server sends the whole list (WORN); a class new to it is made in
// the player's inventory, marked worn only (inventory/stock_projection.hpp: no stock keeps or takes
// it, no scan reports it) and put on once; a class gone from it is taken off and away. The player
// may take a piece off by hand; it is not put on again until the list changes. Everyone else sees
// the pieces through the player's outfit.
//
// Engine thread only.
#pragma once

#include <cstdint>
#include <vector>

#include "kcd2s/integration/hands.hpp"

namespace kcds::runtime::appearance::worn_only {

using Logger = void (*)(const char* format, ...);

void install(Logger logger);

// Makes the local player wear the list of the given revision (a new revision is a new list).
void keep(void* actor, void* entity, const std::vector<kcd2s::integration::ItemClassId>& wanted,
          std::uint64_t revision);

// The connection went: the pieces made for it go too.
void reset(void* actor);

}  // namespace kcds::runtime::appearance::worn_only
