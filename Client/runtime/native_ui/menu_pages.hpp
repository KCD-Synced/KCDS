// The pages of the game's menu (the inventory screen's owner: inventory, Player, map and the
// others), opened and closed as their keys do (WHGame 1.5.5).
//
// Each page's key callback (open_apse_inventory_keyboard, open_apse_map_keyboard,
// open_apse_player, ...) is a std::function whose captured owner, at + 8, is the screen's owner;
// the owner keeps itself + 16 at services + 432. The callbacks share one prologue and differ only
// in the page they pass on (mov dl, page): inventory 2, Player 4, map 6. The menu's open goes
// through the page gate (native_ui.cpp), so a page a resource disabled stays closed.
//
// Engine thread.
#pragma once

#include <cstdint>

namespace kcds::runtime::native_ui::menu_pages {

// Opens the page, as its key does; false when the game has no key callback for it or its screen
// owner is not there yet.
bool open(std::uint8_t page);

// Closes the open menu screen, as its close key (Esc) does; false without one.
bool close();

// The screen's owner as the key callbacks capture it, or null.
void* screen_owner();

}  // namespace kcds::runtime::native_ui::menu_pages
