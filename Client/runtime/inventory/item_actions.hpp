// The client resources' Inventory.use and Inventory.useState: an item of a row of the local
// player's stock used as the game uses it, and whether the game would use it now.
//
// Food and potions: the actor's own use (actor vtable + 1560, the way the food quickslot eats),
// which the server decides as every eating and drinking (inventory_use.hpp: playerConsuming, then
// the game's own effects). Documents the game reads (IsReadable: an item of kind 8 that is not
// of document kind 2): the game's inventory read (ReadItem: where reading is allowed, the reading
// action's own checks with the game's text key of why not, then the reading screen). Every other
// item: the inventory screen's primary action, the one its hint bar names (use, apply, equip), on
// the screen's panel of the player's own items, which is there while the screen is open.
// The state is the screen's own for that action while the screen is open: whether it is offered
// and enabled, the game's text key of why not (cant_use_in_combat, ui_hud_cantReadHere,
// cant_use_quest_item, ...) and the action's label (ui_invhint_read, ...). With the screen
// closed, food is offered as eaten and a document as read where the game lets the player read.
//
// The calls are synchronous requests ({ op: 'syncRequest', id, request: { op: 'itemUse' |
// 'itemUseState', row } }, Client/resources/inventory.cjs), answered on the engine frame.
#pragma once

#include <cstdint>
#include <functional>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::inventory::actions {

using Logger = void (*)(const char* format, ...);
using RuntimeSender = std::function<bool(const kcd2s::sdk::Value&)>;

struct Natives {
  void* primary = nullptr;        // (&panel, &&item), the hint bar's action
  void* primary_state = nullptr;  // (panel, state, item)
  void* item_owner = nullptr;     // (item) -> the actor whose item it is
  void* string_assign = nullptr;  // CryString assign from text
  void* string_init = nullptr;    // an empty CryString
  void* string_free = nullptr;    // a CryString's release
  void* is_readable = nullptr;    // (unused, item) -> bool
  void* read_item = nullptr;      // (human, item, &error text) -> bool
  void* can_read = nullptr;       // (human) -> bool: reading is allowed where they are
  void* (*local_human)() = nullptr;
  // The inventory screen's panel of the player's own items, or null.
  void* (*player_panel)() = nullptr;
  Logger log = nullptr;
};

void install(const Natives& natives);

// A resource runtime message; false when it is not an item action request. Any thread.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Answers the requests (engine thread, in a world).
void frame(void* local_actor);

// Sends the answers to the runtime (network_mutex held).
void pump(const RuntimeSender& send);

}  // namespace kcds::runtime::inventory::actions
