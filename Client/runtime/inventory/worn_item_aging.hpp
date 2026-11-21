// The game's aging of a person's worn items over passing time (a time skip's wait or sleep, for
// everyone around): for each worn item (C_Item + 96, flag 0x10) it finds the item's record in the
// person's list of worn-item records (248 bytes each, the item's GUID at + 64) by the item's GUID
// (C_Item + 56) and copies that record without checking it was found. An item worn without a
// record -- a piece KCDS put on a body, such as an NPC dressed again by the server -- made it copy
// from past the list's end and crash. The person is left out of that one aging step instead: their
// items do not age by it, nothing else changes.
#pragma once

namespace kcds::runtime::inventory::worn_aging {

using Logger = void (*)(const char* format, ...);

// Guards the aging step (found by signature); false when it was not found or not hooked.
bool install(void* aging_step, Logger logger);

}  // namespace kcds::runtime::inventory::worn_aging
