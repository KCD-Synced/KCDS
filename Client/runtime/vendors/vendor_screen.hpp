// A server vendor on the game's own shop screen (Vendor;
// kcd2s/integration/vendor.hpp).
//
// The game trades through a shop (wh::shopmodule::C_Shop): its goods are a game inventory (the
// shop + 224), the shop module finds it by its key (+ 208) through, and C_Shop::Open
// (its virtual slot 7: the keeper's WUID, kept at + 392) brings the shop screen
// (wh::guimodule::C_UIShop, its I_UIShop at services + 440) up with the player's inventory on one
// side and the shop's on the other. What the player moves there is only a basket; the screen's
// Trade hands it to a shopping transaction (C_ShoppingTransaction, keyed by the shop's key), whose
// execute moves the items and the money.
//
// A server vendor is one shop of our own, made once by the game's own constructor and
// reused for every session under a new key each time:
//
//   - its goods are the inventory of a container entity the client stands out of sight
//     ("kcds_vendor_shelf", the game's StashCorpse), filled with the server's shelf rows and a
//     money row holding the purse (what the screen checks the vendor can pay with);
//   - the shop module's lookup by key is hooked: our key answers our shop;
//   - the price calculator's shop price (its slot 6, the price of one unit; the screen
//     and the transaction multiply by the units) is hooked for our shop: a shelf row costs the
//     server's price, a player's item is worth what the vendor's buy list pays for its class, and
//     nothing when the vendor does not buy it;
//   - the transaction's haggle start answers none for our shop, so Trade goes straight
//     to the transaction's accept, unless the server's vendor haggles: then the
//     game's own haggle runs (the dialogue SMLOUVANI with the trader, Negotiation.lua and the
//     dialogue's haggle extension wh::dialogmodule::C_DialogueHaggleExtension) and the server
//     answers every offer:
//       - the extension's input (its slot 6: the price the player chose, 0x80000000
//         when cancelled) is held and the offer goes as VENDOR_HAGGLE;
//       - the server's answer (VENDOR_HAGGLE_REPLY) is what the dialogue reads: the global
//         variables of the trader's reaction (negotiation_merchantReaction_kind, the prices and
//         the impatience of the haggle panel) are written into the game's variable store (services
//         + 0: a vector of { float value, name hash }, its index by name), Lua's
//         Variables.SetGlobal keeps them while the held input goes on, and Lua's
//         Variables.GetLocal answers negotiation_result_price, the price the dialogue
//         accepts with (Shops.AcceptTransaction), with the agreed price;
//   - the transaction's execute is hooked for our shop: nothing moves; its basket (+ 152, the
//     game's items: those of our inventory are bought shelf rows, the player's are their server
//     rows sold, their units at + 0x50) is reported as VENDOR_TRADE;
//   - the screen's state (the I_UIShop + 24 property, 0 when it is down) and the shop it shows
//     (+ 4616) are read each frame: our screen gone is reported as VENDOR_CLOSED. The server's
//     close calls the screen's own close (on the C_UIShop).
//
// Engine thread, except pump (the network worker).
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "kcd2s/integration/vendor.hpp"
#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::vendors {

using Logger = void (*)(const char* format, ...);
using RuntimeSender = std::function<bool(const kcd2s::sdk::Value&)>;

struct Natives {
  void* (*services)() = nullptr;
  void* (*local_entity)() = nullptr;
  // The engine entity of a server NPC by its network id, or null; and an entity's WUID.
  void* (*npc_entity)(std::uint32_t network_id) = nullptr;
  // A player's body on this machine by network id (a player's trade keeps its seller).
  void* (*player_entity)(std::uint32_t network_id) = nullptr;
  bool (*wuid_of)(void* entity, std::uint64_t& wuid) = nullptr;
  void* shop_construct = nullptr;  // C_Shop's constructor
  void* shop_open = nullptr;       // C_Shop::Open(shop, keeper WUID)
  void* shop_by_key = nullptr;     // the shop module's shop by key, hooked
  void* item_price = nullptr;      // the price calculator's shop price, hooked
  void* trade_haggle = nullptr;    // the transaction's haggle start, hooked
  void* trade_execute = nullptr;   // the transaction's execute, hooked
  void* screen_close = nullptr;    // C_UIShop's close
  // The game's haggle (0.4.1); without them a vendor that haggles trades at its list prices.
  void* haggle_input = nullptr;     // the haggle extension's input, hooked
  void* set_global = nullptr;       // Lua's Variables.SetGlobal, hooked
  void* get_local = nullptr;        // Lua's Variables.GetLocal, hooked
  void* variable_index = nullptr;   // the variable store's index by name
  void* end_with_number = nullptr;  // a script call's number result
};

bool install(const Natives& natives, Logger logger);

// What the server sent: a shelf (with `open` a screen to bring up), a close or a haggle answer.
void apply(const kcd2s::integration::VendorOrder& order);

// Brings a screen up that the server asked for and notices one that went away.
void frame(bool in_world);

// The baskets the local player confirmed and the screens that went away on their side since the
// last call, oldest first.
std::vector<kcd2s::integration::VendorTrade> take_trades();
std::vector<kcd2s::integration::VendorClosed> take_closes();
// The prices the local player offered in a vendor's haggle since the last call, oldest first.
std::vector<kcd2s::integration::VendorHaggle> take_haggles();

// The world changed (`connection_over` false) or the connection ended: a screen that was up is
// over, without touching the game's screen.
void reset(bool connection_over);

// The client resources' vendorOpened and vendorClosed, as { op: 'vendorEvent', session, name:
// 'opened' | 'closed', npc, reason }.
void pump(const RuntimeSender& send);

}  // namespace kcds::runtime::vendors
