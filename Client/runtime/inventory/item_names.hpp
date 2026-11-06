// What the game calls items (Inventory.renameItem): the server's names replace the
// text of the items' localization keys, wherever the game names them (inventory, shops, loot).
//
// The game turns a localization key ("@ui_nm_longSwordDuel") into text with its localization
// manager's LocalizeLabel (CLocalizedStringsManager, ILocalizationManager slot 23, WHGame 1.5.5
// manager, label, CryString* out, english), which every "@key" of the UI goes
// through. The branding hook on it asks this first (branding::set_label_override): a key the server
// named gets that name, as its own text would have been written (CryString assign); every other
// key goes on to the game.
//
// apply on the engine thread; the hook on whichever thread the game localizes on.
#ifndef KCDS_RUNTIME_INVENTORY_ITEM_NAMES_HPP
#define KCDS_RUNTIME_INVENTORY_ITEM_NAMES_HPP

#include <string>

#include "kcd2s/integration/inventory.hpp"

namespace kcds::runtime::inventory {

using Logger = void (*)(const char* format, ...);

// string_assign: the CryString assign (string*, const char*). False when it is missing.
bool install_item_names(void* string_assign, Logger logger);

// Takes the server's names: a full set replaces every name before it.
void apply_item_names(const kcd2s::integration::ItemNames& names);

// The name the server gave a localization key (without the '@'); false when it gave none.
bool item_name_of(const std::string& key, std::string& name);

}  // namespace kcds::runtime::inventory

#endif  // KCDS_RUNTIME_INVENTORY_ITEM_NAMES_HPP
