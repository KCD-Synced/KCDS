// The local player's game shows the inventory the server holds (part 2).
//
// The server sends the whole stock (rows: class, amount, quality, health) whenever it changed;
// this brings the game's own inventory to it with the game's own item creation and removal, the
// natives a resource's book already uses (the entity's inventory, ItemDesc,
// add, remove):
//
//   - every row is shown by the game's items of its class with its quality and health: one item
//     holding all units for a class whose units stack, one item per unit otherwise;
//   - an item is made with its quality and health given (ItemDesc + 144 and + 104), so the game
//     keeps both as they are (with quality 0 it would work quality out from health and cap it at
//     3);
//   - items this keeps for a row and no longer needs go; items it never assigned stay, except on
//     the first stock of a connection that a resource restored, when everything it does not hold
//     goes (what the save started with is not the player's); a first stock that grew from the
//     census leaves what the game gained since (a character creator's clothes) to be reported;
//   - what the game then changes on its own (scan) is reported, so the server's stock follows:
//     units spent or dropped, items worn down or mended, items picked up, and how many units of
//     each row the body wears;
//   - when the server changes how many units of a row the body wears (a restore), the body puts
//     them on or takes them off through the game's own equipment; a stock that merely repeats
//     the server's count leaves the body as it is, so the player's own changes stand until
//     their report arrives.
//
// When the player comes into the world, before any stock arrived, the census reports what the
// game's inventory holds; the server takes it as the stock unless a resource set one, so a player
// keeps what they started with.
//
// An item's class object is at + 0x48 (its GUID at + 8; the item's own GUID is at + 0x38), its
// amount at + 0x50, its health at + 0x54, its handle at + 0x30; its quality comes from its quality
// component (+ 0x58, slot 3). An inventory lists its items through + 232: slot
// 4 makes an iterator, whose slot 1 gives the next item slot (null at the end) and slot 0 releases
// it (as the game's GetInventoryTable does).
//
// Engine thread.
#ifndef KCDS_RUNTIME_INVENTORY_STOCK_PROJECTION_HPP
#define KCDS_RUNTIME_INVENTORY_STOCK_PROJECTION_HPP

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "kcd2s/integration/inventory.hpp"

namespace kcds::runtime::inventory {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* (*services)() = nullptr;
  void* (*local_entity)() = nullptr;
  void* inventory_find = nullptr;  // (inventories, entity)
  void* item_desc_init = nullptr;  // (desc, guid*, amount, health)
  void* item_desc_free = nullptr;
  void* inventory_add = nullptr;     // (inventory, desc, reason, flag)
  void* inventory_remove = nullptr;  // (inventory, item, reason, amount)
  void* item_quality = nullptr;      // (item)
  // What the body wears (appearance/outfit.hpp worn and wear).
  void* (*local_actor)() = nullptr;
  std::vector<void*> (*worn)(void* actor) = nullptr;
  bool (*wear)(void* actor, void* item, bool on) = nullptr;
};

bool install(const Natives& natives, Logger logger);

// What the local player's inventory holds, a row per item (rows without ids). Empty without one.
std::vector<kcd2s::integration::InventoryRowState> census();

// Brings the local player's inventory to the stock; false when the player has no inventory.
bool show(const kcd2s::integration::InventoryShow& stock);

// What the game changed on its own since the last stock or scan: units of a row gone (eaten,
// drunk, applied, fired, dropped), an item of a row worn down or mended, and items nobody keeps
// (picked up, or made by the game). Empty before the first stock of a connection.
std::vector<kcd2s::integration::InventoryNativeChange> scan();

// Brings another entity's inventory to rows (a victim's replica, for the thief's own loot screen):
// every item its body does not wear goes, and each row is made anew. False without
// an inventory.
bool fill_pockets(void* entity, void* actor,
                  const std::vector<kcd2s::integration::InventoryRowState>& rows);

// The body wears again what the last stock asks of it, after the game took some of it off on its
// own (restoring a bow to a quickslot takes its arrows off); with `quickslotted` the
// weapons the quickslots hold as well (a body put on another skeleton wears nothing).
void settle_worn(bool quickslotted = false);

// The connection ended or the world changed: the next stock is the first again.
void reset();

// The server's row an item of the game (by its handle) shows, and the revision of the stock the
// game shows; false for an item no row keeps.
bool row_of(std::uint64_t handle, std::string& row, std::uint64_t& revision);

// The game's items that show a row, in the inventory's order; empty for a row no item shows.
std::vector<void*> items_of(const std::string& row);

// Takes one unit of an item of the local player's inventory away, as a script would; the next
// scan reports it. False without the inventory.
bool take_unit(void* item);

// Marks an item of the local inventory as worn only (Player.wearItem), or not any more:
// stocks leave it alone and scans do not report it.
void set_worn_only(void* item, bool worn_only);

// Fills another inventory (a container the server holds) with the rows:
// what it held goes, then every row is made as on the player's own. The rows by the handles of
// the items that show them. What `wearer` (an actor whose own inventory it is: a dead NPC's body)
// wears stays, so the body keeps its clothes.
std::unordered_map<std::uint64_t, std::string> fill_container(
    void* inventory, const std::vector<kcd2s::integration::InventoryRowState>& rows,
    void* wearer = nullptr);

// The game's own inventory of an entity (a container the client stands itself, a vendor's shelf);
// null without one.
void* inventory_of_entity(void* entity);

// The game's own inventory of the local player; null without one.
void* local_player_inventory();

// The items an inventory holds, in its order (empty without the natives).
std::vector<void*> items_in(void* inventory);

// The handle and class of every item of the local player's inventory; empty without one.
std::vector<std::pair<std::uint64_t, kcd2s::integration::Guid>> local_item_classes();

}  // namespace kcds::runtime::inventory

#endif  // KCDS_RUNTIME_INVENTORY_STOCK_PROJECTION_HPP
