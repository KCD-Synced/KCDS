// The inventory screen and the player's horse that is no longer there, WHGame 1.5.5.
//
// While the inventory is open the game asks how much the player's horse can still carry, to say
// whether an item would fit in its saddlebags: the inventory's helper (sub_18055E3C0) takes the
// companion manager's first companion (sub_1808ACACC(manager, 0), its + 1640 the soul) and reads
// its capacity (derived stats 23 and 40). The manager can still list a horse whose body is gone,
// such as a server horse taken back from the player as the inventory opened, and the companion
// came back null: an access violation at + 0x20 (1.0.9 crash reports). The helper is hooked: with
// no companion it answers 0 (nothing fits), else the game's own.
//
// Engine thread.
#pragma once

namespace kcds::runtime::riding::horse_inventory_guard {

using Logger = void (*)(const char* format, ...);

// free_capacity: the helper's address (its call of the companion getter at + 0x18). False when
// it could not be hooked.
bool install(void* free_capacity, Logger logger);

}  // namespace kcds::runtime::riding::horse_inventory_guard
