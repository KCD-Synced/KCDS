// The client Inventory.whenUsed overrides: a resource takes over an action of the
// local player's native inventory screen for chosen items, and gets the use instead of the game.
//
// The runtime names the overridden item classes and native instances (UIDs)
// ({ op: "inventoryUseOverrides", classes: [GUID text], uids: [decimal text] }). Three actions of
// the inventory screen are hooked (WHGame 1.5.5 addresses):
// - "secondary": eat, drink, apply (the screen controller's consume: controller, item,
//   skip confirmation) and learn (controller, item);
// - "primary": the action the hint bar names, on Enter or a double click: equip, read, use,
//   apply, pouch (&controller, &&item);
// - "activate": the paper doll's equip toggle, an item moved onto a slot or back to the list
//   (controller, item id string, slot name, target), whose item the controller's
//   lookup (vtable + 200) finds.
// For an overridden item that the server's stock shows (a row of it), the game's action does not
// run and the runtime hears { op: "inventoryUse", item, uid, id, revision, amount: 1, action } on
// the next pump, unless the row or the stock's revision changed by then; every other item goes to
// the game. A press on one item counts once per frame. The primary action's state of
// an overridden item is always offered and enabled, labelled Use when the game has no label.
//
// A use no resource took over that eats, drinks or applies an item of a row (the consume path) is
// the server's to decide: the game does nothing yet, the client asks (Consume), and on
// the verdict uses the item as the game does (its owner's use, then vtable + 1560, as
// the consume path itself does), uses it up without the game's own effects (one unit removed), or
// leaves it; the receipt says whether it was used.
//
// Hooks on the engine thread; handle_use_message on the network thread.
#ifndef KCDS_RUNTIME_INVENTORY_INVENTORY_USE_HPP
#define KCDS_RUNTIME_INVENTORY_INVENTORY_USE_HPP

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "kcd2s/integration/inventory.hpp"

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::inventory {

using Logger = void (*)(const char* format, ...);
using RuntimeSender = std::function<bool(const kcd2s::sdk::Value&)>;

struct UseNatives {
  void* consume = nullptr;
  void* learn = nullptr;
  void* primary = nullptr;
  void* equip_toggle = nullptr;
  void* primary_state = nullptr;
  void* string_copy = nullptr;    // CryString copy
  void* string_assign = nullptr;  // CryString assign from text
  void* item_owner = nullptr;     // the actor whose item it is
  // A serving from a cooking pot or a barrel: the soul script binding's EatItem (bind,
  // function handler, class GUID text) makes one unit of the class in the soul's inventory and
  // uses it as the consume path does. Hooked for the local soul: the server decides.
  void* eat_item_bind = nullptr;
  void* handler_soul = nullptr;     // the soul a script call is made on
  void* script_end_bool = nullptr;  // a script function's boolean result
  void* soul_owner = nullptr;       // the actor of a soul
  void* item_desc_init = nullptr;
  void* item_desc_free = nullptr;
  void* inventory_add = nullptr;
  void* inventory_remove = nullptr;
  void* (*local_soul)() = nullptr;
  // The actor's use of an item (actor vtable slot 195: owner, &result, item, units,
  // ...), which eating from a food quickslot goes through too: hooked for the local player's food
  // and potions of the server's rows, which the server decides as in the inventory screen.
  void* owner_use = nullptr;
};

// False when one is missing or cannot be hooked.
bool install_use(const UseNatives& natives, Logger logger);

// For local::Client::set_local_runtime_handler: true when the message was the overrides.
bool handle_use_message(const kcd2s::sdk::Value& message);

// Hands the uses taken over since the last call to the runtime.
void pump_uses(const RuntimeSender& send);

// The uses to ask the server about since the last call.
struct ConsumeRequest {
  std::uint32_t request = 0;
  std::string row;
  std::uint64_t revision = 0;
  // A serving from a pot or a barrel: its class (and no row).
  bool pot = false;
  kcd2s::integration::Guid item{};
};
std::vector<ConsumeRequest> take_consume_requests();

// The use action chosen on an item of a usable type a resource registered, which no
// client handler took: the server raises playerCustomItemUse. The game does nothing itself.
struct CustomUse {
  std::string row;
  std::uint64_t revision = 0;
};
std::vector<CustomUse> take_custom_uses();

// Carries out the server's verdict on a use; whether the game used the item (the receipt), or
// nothing for a verdict there is nothing to report on (unknown, or refused).
std::optional<bool> apply_consume_verdict(const kcd2s::integration::InventoryConsume& verdict);

// The world changed or the connection ended: uses waiting for a verdict are dropped, so a late
// verdict never runs on another body or in another world.
void reset_uses();

}  // namespace kcds::runtime::inventory

#endif  // KCDS_RUNTIME_INVENTORY_INVENTORY_USE_HPP
