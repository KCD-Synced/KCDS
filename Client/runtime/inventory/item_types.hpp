// Item types a resource registered (Items.register), made in the game.
//
// The game finds an item's class by its GUID with one function (the class registry
// &GUID; a binary search of its {GUID, class*} list at + 120, its count at + 128),
// which item creation, the inventory and the UI go through. It is hooked: a registered type's GUID
// answers the type's own class.
//
// A type's class is a copy of a miscellaneous item's class (0xE0 bytes): the visual's own when it
// is a miscellaneous item or a crafting material (their slot 7, the stacking, reads + 0xB0), else a
// reference miscellaneous item's with the visual's model (+ 0x38) and icon (+ 0x90). The copy
// takes the type's GUID (+ 0x08), id (+ 0x18), weight (+ 0x58, float), price (+ 0x5C), description
// and name keys (+ 0x98, + 0xA0; ITEM_NAMES carries their texts) and stacking (+ 0xB0); a type with
// a model of its own has it at + 0x38 and an empty material (+ 0x40). Its
// strings are CryStrings (the text after a header: reference count, length, capacity), never
// freed.
//
// The client's resource runtime hears the registered types' ids and GUIDs, so that
// Inventory.whenUsed takes them as selectors, and their icons, so that it ships each icon's .dds
// for the game to load ({ op: "itemTypes", types: [{ id, item, icon, iconFile }] }, the whole set
// on every change and to each new session's runtime). A type with its own icon has the class
// copy's icon id (+ 0x90) set to it: the inventory loads
// Libs/UI/Textures/Icons/Items/<icon>_icon.dds.
//
// apply on the engine thread; pump_types on the network thread; the hook on whichever thread the
// game looks a class up.
#ifndef KCDS_RUNTIME_INVENTORY_ITEM_TYPES_HPP
#define KCDS_RUNTIME_INVENTORY_ITEM_TYPES_HPP

#include <functional>
#include <string>

#include "kcd2s/integration/item_types.hpp"

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::inventory {

using Logger = void (*)(const char* format, ...);
using RuntimeSender = std::function<bool(const kcd2s::sdk::Value&)>;

// lookup: the game's class lookup. False when it is missing or cannot be hooked.
bool install_item_types(void* lookup, Logger logger);

// Makes the types: a full set replaces every type before it. Types that come before the game
// looked up any class wait for its first lookup; an empty change makes the waiting ones.
void apply_item_types(const kcd2s::integration::ItemTypes& types);

// The game's own class of a GUID (not a registered type's), or null before the game looked any
// class up. Any thread.
void* game_item_class(const kcd2s::integration::Guid& id);

// The icon name of an item (Libs/UI/Textures/Icons/Items/<icon>_icon.dds) by its GUID, a
// registered type's id or a game item's name (any case); "" for an unknown item or before the
// game looked any class up. A registered type answers its own icon, or its visual's. Any thread.
std::string item_icon_name(const std::string& name_or_guid);

// Whether an item class is one of the registered types that are usable; its use action's label
// key then. Any thread.
bool usable_type(const void* item_class, std::string& use_label_key);

// Sends the registered types to the session's resource runtime when they changed since it last
// heard them, or the session is new.
void pump_types(const void* session, const RuntimeSender& send);

}  // namespace kcds::runtime::inventory

#endif  // KCDS_RUNTIME_INVENTORY_ITEM_TYPES_HPP
