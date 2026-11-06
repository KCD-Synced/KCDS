// The local player's quickslots: what the current outfit's weapon and item quickslots
// hold, by the server's rows, and a restore of them.
//
// Each outfit keeps two quick access managers (C_QAMManager, WHGame 1.5.5): the outfit manager is
// the soul's equipment (actor + 1640, + 408) at + 272, + 88; its current outfit is the byte at
// + 1760, and outfit n's block at + 136 + 544 n holds the weapons' manager and, 256 bytes on, the
// items'. A manager's slot i is at + 48 + 48 i: its primary item, then its secondary; + 240 is the
// selected slot and + 244 how many slots the outfit can use (belts and sheaths add them). Its
// vtable: + 40 selects a slot, + 176 puts an item into a slot (primary or secondary by the item's
// kind, the game's own EquipPlayersItem path), + 272 says whether a slot can be used.
// The manager an item belongs in comes from the equipment manager's owner (+ 8): its vtable
// + 480, then + 392, then + 144 with the item, as asks.
//
// Engine thread.
#ifndef KCDS_RUNTIME_INVENTORY_QUICKSLOTS_HPP
#define KCDS_RUNTIME_INVENTORY_QUICKSLOTS_HPP

#include "kcd2s/integration/inventory.hpp"

namespace kcds::runtime::inventory {

using Logger = void (*)(const char* format, ...);

// What the actor's current outfit's quickslots hold, by the rows of the stock the game shows (""
// for a slot that is empty or holds an item no row keeps); false when there are none to read.
bool read_quickslots(void* actor, kcd2s::integration::InventoryQuickslots& slots);

// Puts the rows' items into the actor's current outfit's quickslots and selects the slots. A slot
// the outfit cannot use, an empty reference and an item that belongs in the other kind of slot
// are left as the game has them, and logged with the reason when `log_line` is given;
// `unusable` counts the slots the outfit cannot use (yet: a belt put on a moment ago adds its slots
// a little later). False when there are no quickslots.
bool restore_quickslots(void* actor, const kcd2s::integration::InventoryQuickslots& slots,
                        Logger log_line, unsigned& unusable);

}  // namespace kcds::runtime::inventory

#endif  // KCDS_RUNTIME_INVENTORY_QUICKSLOTS_HPP
