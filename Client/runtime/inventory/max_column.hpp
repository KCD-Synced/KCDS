// The inventory's max column, WHGame 1.5.5: the most units of each class the server
// lets a player hold, in the game's own inventory list after its amount column (#), under the
// max carry icon (icon 10 of the set: a hash between brackets).
//
// The list is the game's movie Libs/UI/ApseInventoryList.gfx (element ApseInventoryList), left
// as the game ships it; a container's screen shows its two lists in Libs/UI/ItemTransfer.gfx
// (element ItemTransfer, instances 0 and 1), whose rows are drawn by the same ItemView. While the
// server limits any class, a small movie of the client's own is loaded into the list's
// _root.kcdsMax (NativeUI clip calls: create, then loadMovie); its ActionScript wraps the list's
// row drawing (ItemView.m_setDataFunc and colorizeName): each row moves its equip and amount cells
// one column to the left and shows its class's limit where the amount was, and the sort bar gets
// the icon there. Classes without a limit leave the cell empty; a server without limits never loads
// the movie, and an empty set puts the columns back.
//
// The movie is kept in memory as one of the client's own files (native_ui::write_own_file) and
// loaded as /%USER%/kcds_ui/_kcds/..., which the movie loader opens through NativeUI's file hook.
//
// Engine thread.
#ifndef KCDS_RUNTIME_INVENTORY_MAX_COLUMN_HPP
#define KCDS_RUNTIME_INVENTORY_MAX_COLUMN_HPP

#include <cstdint>
#include <map>
#include <string>

#include "kcd2s/integration/guid.hpp"

namespace kcds::runtime::inventory {

using Logger = void (*)(const char* format, ...);

// panel_bind: the menu's binding of a UI element to a 3D panel (binder, element name,
// panel entity, panel name, flags), hooked to learn the instances the list is shown in (each its
// panel entity's id: the menu opens "ApseInventoryList@<id>.ui" for the inventory, the shop's
// player and trader sides and a container). False when it cannot be hooked.
bool install_max_column(void* panel_bind, Logger log);

// The server's limits by class (none: the column goes) and the column's tooltip in the player's
// language (the game's tooltip shows it under the localization key kcds_tip_max_carry).
void set_max_column_limits(std::map<kcd2s::integration::Guid, std::uint32_t> limits,
                           const std::string& tooltip);

// Every frame: loads the movie into an open inventory list and hands it the limits.
void tick_max_column(Logger log);

}  // namespace kcds::runtime::inventory

#endif
