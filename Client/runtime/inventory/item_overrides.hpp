// What the server changes of the game's own items, WHGame 1.5.5.
//
// An item class keeps its weight at + 0x58 (a float) and its price at + 0x5C (whole groschen), the
// fields the game's inventory weight, tooltips and trade read (item_types.hpp writes the same
// fields of a registered type's class). The server sends the whole set of its overrides
// (ITEM_OVERRIDES); each class it names gets the server's values, and a class a newer set no
// longer names gets the game's own back. A set that comes before the game looked up any class
// waits for it.
//
// The player's soul keeps its inventory's weight among its derived values (C_Soul + 0xB50, the
// weight at + 0x1C), summed from the classes' weights only when they are stale. A set
// that changed classes tells the local soul so through its base at + 8 (vtable slot 3:
// bumps the revision at + 0xB98 and clears the computed flags at + 0xB96), as an inventory change
// does; the encumbrance then follows at once.
//
// Stats (armour defense, weapon attack and requirements, food effects, visibility, charisma) are
// fields of the class too, each at its own offset (item_overrides.cpp, kStatFields); the server
// sends only those an item's category carries. Every field a set changes keeps the class's own
// four bytes, which a newer set or leaving the world writes back.
//
// The limits (the most units of a class a player may hold) go to the inventory's max column
// (max_column.hpp).
//
// Engine thread only.
#ifndef KCDS_RUNTIME_INVENTORY_ITEM_OVERRIDES_HPP
#define KCDS_RUNTIME_INVENTORY_ITEM_OVERRIDES_HPP

#include "kcd2s/integration/item_overrides.hpp"

namespace kcds::runtime::inventory {

using Logger = void (*)(const char* format, ...);

// A new set from the server; soul is the local player's (null outside the world).
void apply_item_overrides(const kcd2s::integration::ItemOverrides& overrides, void* soul,
                          Logger log);

// Every frame: a set waiting for the game's classes is applied once they are there.
void keep_item_overrides(void* soul, Logger log);

// The world was left: every class gets the game's own values back.
void reset_item_overrides();

}  // namespace kcds::runtime::inventory

#endif
