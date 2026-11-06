// Names of single rows: a weapon, a piece of armour or any other row of the local
// player's inventory that the server names on its own shows that name and description, while the
// item stays of its own class for everything the game does (equipping, combat, wear, the outfit).
//
// The game names an inventory entry from its item's class, not from the item (WHGame 1.5.5):
//
//   - a list entry (C_UIFlashListItem, the base of the trader's and the loot screen's entries) is
//     made (entry, item, pricer): its C_UIFlashObject base takes "Id" (+ 8) from the
//     item's handle (+ 0x30) and "Name" (+ 16) from the class's name key (class + 0xA0);
//   - the item panel (C_UIFlashInfoItem, the base of every category's panel) is made
//     (panel, item, pricer): "Name" (+ 16) the same way and the description (+ 40) from the
//     class's description key (class + 0x98).
//
// Both are hooked: after the game made one for an item that shows a row (stock_projection row_of),
// a row the server named (item_names: kcds_nm_r_<row>, kcds_in_r_<row>, which only that player
// hears) gets the server's text for them instead. The entry's strings are shown as written, not
// looked up as localization keys (a key showed as written), so the text goes in
// itself.
//
// Engine thread (the UI is filled there).
#ifndef KCDS_RUNTIME_INVENTORY_ROW_NAMES_HPP
#define KCDS_RUNTIME_INVENTORY_ROW_NAMES_HPP

namespace kcds::runtime::inventory {

using Logger = void (*)(const char* format, ...);

struct RowNameNatives {
  void* list_entry = nullptr;     // (entry, item, pricer)
  void* info_panel = nullptr;     // (panel, item, pricer)
  void* string_assign = nullptr;  // CryString assign (string*, const char*)
};

bool install_row_names(const RowNameNatives& natives, Logger logger);

}  // namespace kcds::runtime::inventory

#endif  // KCDS_RUNTIME_INVENTORY_ROW_NAMES_HPP
