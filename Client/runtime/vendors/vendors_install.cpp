// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "vendors_install.hpp"

#include <cstdint>
#include <cstring>

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "vendor_screen.hpp"
#include "vendors_tick.hpp"

namespace kcds::game_client {

void install_vendors(const kcds::runtime::book::Natives& natives) {
  // The server's vendors on the game's own shop screen: a C_Shop of our
  // own (its constructor, C_Shop::Open), the shop module's lookup by key
  // the price calculator's shop price, the shopping transaction's
  // haggle start and execute, and the shop screen's close.
  kcds::runtime::vendors::Natives vendors;
  vendors.services = natives.services;
  vendors.local_entity = natives.local_entity;
  vendors.npc_entity = &npc_engine_entity;
  vendors.player_entity = &player_engine_entity;
  vendors.wuid_of = &entity_wuid;
  vendors.shop_construct = resolve_unique(
      "ShopConstruct",
      "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 33 FF 48 8D 05 ?? ?? ?? ?? 48 89 BB "
      "88 00 00 00 48 89 BB 90 00 00");
  vendors.shop_open = resolve_unique(
      "ShopOpen",
      "48 89 5C 24 18 48 89 74 24 20 55 57 41 56 48 8D 6C 24 D0 48 81 EC 30 01 00 00 48 8B 05 ?? "
      "?? ?? ?? 48 33 C4 48 89 45 20 4C 8B F1 48 89 91 88 01 00 00 E8");
  vendors.shop_by_key = resolve_unique(
      "ShopByKey",
      "48 89 54 24 10 53 48 83 EC 20 48 3B 15 ?? ?? ?? ?? 48 8B D9 75 09 48 8B 81 80 00 00 00 EB "
      "38");
  vendors.item_price = resolve_unique(
      "ShopItemPrice",
      "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 54 41 56 41 57 48 83 EC 60 0F "
      "29 70 D8 49 8B C8 F3 41 0F 10");
  vendors.trade_haggle = resolve_unique(
      "ShopTradeHaggle",
      "48 89 5C 24 10 48 89 74 24 18 55 57 41 56 48 8D 6C 24 A0 48 81 EC 60 01 00 00 48 8B 05 ?? "
      "?? ?? ?? 48 33 C4 48 89 45 50 48 8B 01 8A DA 48 8D 54");
  vendors.trade_execute = resolve_unique(
      "ShopTradeExecute",
      "48 89 5C 24 10 55 56 57 48 8D 6C 24 B9 48 81 EC E0 00 00 00 49 8B D8 48 8B F9 44 8B C2 48 "
      "8D 55 67 E8");
  vendors.screen_close = resolve_unique(
      "ShopScreenClose",
      "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 56 41 57 48 8B EC 48 83 EC 30 48 8D 59 "
      "28 4C 8B F1 48 8B 03 48 8B CB");
  // The game's haggle, answered by the server for a vendor that haggles (0.4.1): the dialogue's
  // haggle extension input, Lua's Variables.SetGlobal and GetLocal
  // the variable store's index by name and a script call's number
  // result.
  vendors.haggle_input = resolve_unique(
      "HaggleInput",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 0F 29 74 24 20 41 0F B6 E8 8B "
      "F2 48 8B D9 E8 ?? ?? ?? ?? 48 8B 38 81 FE 00 00 00 80");
  vendors.set_global = resolve_unique(
      "ScriptSetGlobal",
      "48 89 5C 24 08 57 48 83 EC 30 48 8B FA 0F 29 74 24 20 48 8B D9 49 8B D0 48 8B 49 60 0F 28 "
      "F3 E8");
  vendors.get_local = resolve_unique(
      "ScriptGetLocal",
      "40 53 48 83 EC 20 4C 8B D1 45 8B D9 49 8B C8 48 8B DA E8 ?? ?? ?? ?? 49 8B 4A 68 4C 8D 4C "
      "24 30 8B D0 45 8B C3 E8 ?? ?? ?? ?? 84 C0");
  vendors.variable_index = resolve_unique(
      "VariableIndex",
      "40 53 48 83 EC 20 48 8B D9 48 8B CA E8 ?? ?? ?? ?? 48 8B 53 08 44 8B C8 4C 8B 1B 48 8B CA "
      "49 2B CB 33 C0 48 C1 F9 03 85");
  vendors.end_with_number = resolve_unique(
      "ScriptEndWithNumber",
      "40 53 48 83 EC 40 F3 0F 10 02 48 8D 54 24 20 48 8B 01 F3 0F 11 44 24 28 C7 44 24 20 04 00 "
      "00 00 FF 50 50 48 8D 4C 24 20 8B D8 E8");
  log("VENDOR shop screen %s",
      kcds::runtime::vendors::install(vendors, log) ? "installed" : "unavailable");
}

}  // namespace kcds::game_client
