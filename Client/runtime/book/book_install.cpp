// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "book_install.hpp"

#include <cstdint>
#include <cstring>

#include "../appearance/appearance_install.hpp"
#include "../carry/body_carry_install.hpp"
#include "../cookpot/cookpot_install.hpp"
#include "../core/client_state.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../dice/dice_install.hpp"
#include "../gathering/gathering_install.hpp"
#include "../ground/ground_install.hpp"
#include "../hud/notices_install.hpp"
#include "../inventory/inventory_install.hpp"
#include "../markers/markers_install.hpp"
#include "../physics/physics_install.hpp"
#include "../pickpocket/pickpocket_install.hpp"
#include "../quests/quests_install.hpp"
#include "../ranged/aim_install.hpp"
#include "../replicas/replica_guards_install.hpp"
#include "../stash/stash_install.hpp"
#include "../vendors/vendors_install.hpp"
#include "../weapon_hits/weapon_hits_tick.hpp"
#include "../world/world_clock_install.hpp"

namespace kcds::game_client {

kcds::runtime::book::Natives install_book(bool& installed) {
  // Book: the game's reading of an inventory document, with a resource's pages.
  kcds::runtime::book::Natives natives;
  natives.services = get_services;
  natives.local_human = [] { return local_combat_actor.load(); };
  // CGameObjectExtensionHelper<C_Actor, IActor, 64> keeps the engine entity at +56.
  natives.local_entity = []() -> void* {
    auto actor = static_cast<std::uint8_t*>(local_combat_actor.load());
    return actor ? *reinterpret_cast<void**>(actor + 56) : nullptr;
  };
  natives.string_assign = string_assign;
  natives.book_listener =
      resolve_unique("BookListener", "44 89 4C 24 20 F3 0F 11 54 24 18 55 53 56 57 41 56 48 8D 6C");
  natives.inventory_find = resolve_unique(
      "EntityInventory",
      "48 89 5C 24 18 48 89 74 24 20 55 57 41 56 48 8B EC 48 83 EC 40 48 8B 05 ?? ?? ?? ?? 48 33 "
      "C4 48");
  natives.item_desc_init =
      resolve_unique("ItemDescInit", "40 53 48 83 EC 20 0F 10 05 ?? ?? ?? ?? 48 8B 05");
  natives.item_desc_free = resolve_unique(
      "ItemDescFree", "40 53 48 83 EC 20 48 8B D9 48 81 C1 D8 00 00 00 E8 ?? ?? ?? ?? 4C 8B 83");
  natives.inventory_add =
      resolve_unique("InventoryAdd", "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 41 8A F9 48 8B");
  natives.inventory_remove = resolve_unique(
      "InventoryRemove",
      "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 54 41 56 41 57 48 83 EC 30 "
      "4C 8B B2");
  natives.read_item = resolve_unique(
      "ReadItem",
      "48 89 5C 24 20 55 56 57 41 56 41 57 48 8B EC 48 83 EC 70 48 8B 05 ?? ?? ?? ?? 48 33 C4 "
      "48 89 45 F0 49 8B F0");
  natives.string_init = resolve_unique(
      "CryStringInitEmpty",
      "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 83 C0 0C 48 89 03 48 8B C3 48 83 C4 20 5B C3 "
      "CC CC 48 89 5C 24");
  natives.string_free =
      resolve_unique("CryStringFree", "48 83 EC 28 48 8B 09 48 83 C1 F4 E8 ?? ?? ?? ??");
  natives.reading_teardown =
      resolve_unique("ReadingTeardown", "48 89 5C 24 08 57 48 81 EC 10 01 00 00 48 8B D9");
  natives.activity = resolve_unique(
      "PlayerActivity",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 41 8A E9 ?? ?? ?? ?? F2");
  natives.can_read = resolve_unique(
      "CanReadHere",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 01 48 8B F9 FF 90");
  natives.inventory_return = resolve_unique(
      "ReadingInventoryReturn",
      "4C 8B DC 49 89 5B 10 57 48 81 EC D0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 "
      "C0 00 00 00 33 D2");
  natives.document_read = resolve_unique(
      "DocumentRead",
      "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 90 00 00 00 0F 29 70 B8 48 8B 05 ?? ?? "
      "?? ?? 48 33 C4 48 89 44 24 70");
  natives.page_actions =
      resolve_unique("ReadingPageActions",
                     "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8B EC 48 83 EC 40 "
                     "48 8B 01 41 8A F8 44 8A F2 48 8B F1 FF 50 40");
  // tests the UI's text flags (test cs:dword, 400h) at +0x25.
  if (auto localize = static_cast<std::uint8_t*>(resolve_unique(
          "LocalizeWords",
          "48 89 5C 24 18 48 89 7C 24 20 55 48 8D 6C 24 F0 48 81 EC 10 01 00 00 48 8B 05 ?? ?? "
          "?? ?? 48 33 C4 48 89 45 00 F7 05 ?? ?? ?? ?? 00 04 00 00"))) {
    std::int32_t displacement;
    std::memcpy(&displacement, localize + 0x27, 4);
    natives.ui_text_flags = reinterpret_cast<std::uint32_t*>(localize + 0x2F + displacement);
  }
  const bool book = kcds::runtime::book::install(natives, log);
  installed = book;
  return natives;
}

void install_book_and_shared_natives_users() {
  bool book = false;
  const kcds::runtime::book::Natives natives = install_book(book);
  install_outfit(natives);
  install_stock(natives);
  install_ground(natives);
  install_hud_notices();
  install_body_carry();
  install_replica_guards();
  install_game_texts();
  install_aim();
  install_weapon_hits();
  install_pickpocket();
  install_world_physics();
  install_stash(natives);
  install_vendors(natives);
  install_world_clock();
  install_markers();
  install_quests();
  install_dice();
  install_gathering();
  install_cookpot_looks();
  install_item_uses(natives);
  install_appearance_guards(natives);
  log("BOOK %s", book ? "installed" : "unavailable");
}

}  // namespace kcds::game_client
