// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "inventory_install.hpp"

#include "../core/client_state.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../appearance/outfit.hpp"
#include "../riding/native_horses.hpp"
#include "equipped_weight.hpp"
#include "inventory_use.hpp"
#include "item_names.hpp"
#include "max_column.hpp"
#include "item_types.hpp"
#include "row_names.hpp"
#include "stock_projection.hpp"

namespace kcds::game_client {

void install_stock(const kcds::runtime::book::Natives& natives) {
  kcds::runtime::inventory::Natives stock;
  stock.services = natives.services;
  stock.local_entity = natives.local_entity;
  stock.inventory_find = natives.inventory_find;
  stock.item_desc_init = natives.item_desc_init;
  stock.item_desc_free = natives.item_desc_free;
  stock.inventory_add = natives.inventory_add;
  stock.inventory_remove = natives.inventory_remove;
  stock.item_quality = resolve_unique(
      "ItemQuality", "48 8B 49 58 48 85 C9 74 07 48 8B 01 48 FF 60 18 B8 01 00 00 00 C3");
  stock.local_actor = [] { return local_combat_actor.load(); };
  stock.worn = kcds::runtime::appearance::outfit::worn;
  stock.wear = kcds::runtime::appearance::outfit::wear;
  log("INVENTORY stock projection %s",
      kcds::runtime::inventory::install(stock, log) ? "installed" : "unavailable");

  // The share of worn items' weight in the load: the soul's derived values
  // and the lambda that adds an item's weight to them.
  kcds::runtime::inventory::equipped_weight::Natives weight;
  weight.derive =
      resolve_unique("SoulDeriveValues",
                     "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 70 48 8B 05 ?? ?? ?? ?? "
                     "48 33 C4 48 89 44 24 60 80 79 4E 00 48 8B F2 48 8B F9");
  weight.add_weight = resolve_unique("SoulItemWeightAdd",
                                     "40 53 48 83 EC 30 8B 42 50 48 8B D9 0F 29 74 24 20 48 8B CA "
                                     "0F 57 F6 F3 48 0F 2A F0 E8 ?? ?? ?? ?? 48 8B 43 08 F3 0F 59");
  weight.local_actor = [] { return local_combat_actor.load(); };
  weight.worn = kcds::runtime::appearance::outfit::worn;
  weight.log = log;
  kcds::runtime::inventory::equipped_weight::install(weight);
}

void install_item_uses(const kcds::runtime::book::Natives& natives) {
  kcds::runtime::inventory::UseNatives uses;
  uses.consume = resolve_unique("InventoryConsume",
                                "48 89 5C 24 08 48 89 74 24 10 57 41 56 41 57 48 81 EC E0 00 00 00 "
                                "4C 8B F1 41 0F B6 D8 48 8B CA 48 8B FA E8");
  uses.learn =
      resolve_unique("InventoryLearn",
                     "48 89 5C 24 08 55 56 57 41 56 41 57 48 8D 6C 24 C9 48 81 EC 00 01 00 "
                     "00 4C 8B F9 48 8B FA 48 8B CA E8");
  uses.primary =
      resolve_unique("InventoryPrimary",
                     "48 89 5C 24 10 48 89 74 24 18 55 57 41 56 48 8D 6C 24 B9 48 81 EC E0 00 "
                     "00 00 48 8B 02");
  uses.equip_toggle =
      resolve_unique("InventoryEquipToggle",
                     "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 56 41 57 48 8B EC 48 "
                     "83 EC 50 48 8B 01");
  uses.primary_state =
      resolve_unique("InventoryPrimaryState",
                     "48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 50 48 8B E9");
  uses.string_copy =
      resolve_unique("CryStringCopy",
                     "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B D9 48 "
                     "8B 0A");
  uses.string_assign = reinterpret_cast<void*>(string_assign);
  uses.owner_use =
      resolve_unique("OwnerItemUse", "48 89 5C 24 18 48 89 6C 24 20 56 57 41 54 48 83 EC 60");
  uses.eat_item_bind = resolve_unique(
      "SoulEatItemBind",
      "48 89 5C 24 08 48 89 74 24 20 55 57 41 56 48 8D 6C 24 80 48 81 EC 80 01 00 00 48 8B 05 ?? "
      "?? ?? ?? 48 33 C4 48 89 45 70");
  uses.handler_soul = resolve_unique(
      "ScriptHandlerSoul",
      "48 89 5C 24 08 57 48 83 EC 20 48 8D 54 24 38 48 C7 44 24 38 00 00 00 00 E8 ?? ?? ?? ?? 48 "
      "8B 5C 24 38 84 C0 74");
  uses.script_end_bool = resolve_unique("ScriptEndBool",
                                        "40 53 48 83 EC 40 8A 02 48 8D 54 24 20 88 44 24 28 48 8B "
                                        "01 C7 44 24 20 02 00 00 00 FF 50 50");
  uses.soul_owner = resolve_unique(
      "SoulOwner",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 80 FA 02 73 ?? 0F B6 C2 48 8B B4 C1 E0 0C 00 "
      "00 48 85 F6 74 ?? E8 ?? ?? ?? ?? 48 8B CE 48 8B B8 80 01 00 00 48 8B 07 48 8B 58 18 48 8B "
      "06 FF 50 08 8B D0 48 8B CF FF ?? EB ??");
  uses.item_desc_init = natives.item_desc_init;
  uses.item_desc_free = natives.item_desc_free;
  uses.inventory_add = natives.inventory_add;
  uses.inventory_remove = natives.inventory_remove;
  uses.local_soul = [] {
    void* actor = local_combat_actor.load();
    void* soul = nullptr;
    if (actor) {
      std::memcpy(&soul, static_cast<std::uint8_t*>(actor) + 1640, sizeof(soul));
    }
    return soul;
  };
  uses.item_owner = resolve_unique("ItemOwner",
                                   "48 83 EC 28 E8 ?? ?? ?? ?? 33 D2 48 8B 08 4C 8B 81 00 04 00 "
                                   "00 48 8B C8 48 83 C4 28 49 FF E0");
  log("INVENTORY use overrides %s",
      kcds::runtime::inventory::install_use(uses, log) ? "installed" : "unavailable");
  log("INVENTORY item names %s",
      kcds::runtime::inventory::install_item_names(reinterpret_cast<void*>(string_assign), log)
          ? "installed"
          : "unavailable");
  // A row's own name on its list entry and item panel.
  kcds::runtime::inventory::RowNameNatives row_names;
  row_names.list_entry =
      resolve_unique("UiListItemMake",
                     "48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 70 0F 29 "
                     "74 24 60 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 E0 4C 8B F9 4D 8B E0");
  row_names.info_panel =
      resolve_unique("UiInfoItemMake",
                     "4C 89 44 24 18 55 53 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 28 4C "
                     "8B F1 4C 8B FA 48 8B CA E8");
  row_names.string_assign = reinterpret_cast<void*>(string_assign);
  // A piece a server horse wears keeps the name of the row it was put on from.
  row_names.horse_piece_key = &horse_piece_key;
  kcds::runtime::inventory::install_row_names(row_names, log);
  log("INVENTORY item types %s",
      kcds::runtime::inventory::install_item_types(
          resolve_unique("ItemClassLookup",
                         "48 89 5C 24 08 55 56 57 41 56 41 57 48 83 EC 40 48 8B EA"),
          log)
          ? "installed"
          : "unavailable");
  log("INVENTORY max column %s",
      kcds::runtime::inventory::install_max_column(
          resolve_unique("UiPanelBind",
                         "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 20 4C 89 41 "
                         "08 49 8B F9 48 8B F2 48 8B E9 4D 85 C0 0F 84 ?? ?? ?? ?? 48 8B 0D"),
          log)
          ? "installed"
          : "unavailable");
}

}  // namespace kcds::game_client
