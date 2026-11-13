// Outfit synchronization: a player's replica wears what its owner
// wears and carries the weapons its owner has equipped.
//
// The owner reports the classes of the clothing it wears in the replicated NetworkPlayer field
// "outfit" (capture). Every other client makes the replica's worn clothing the same (apply):
// pieces the owner does not wear are taken off and out of the replica's inventory, missing ones
// are created in it and put on. Everything runs through the game's own equipment (WHGame 1.5.5):
//
//   soul + 408                the soul's equipment interface (soul vtable slot 60)
//     vtable slot 40 / 41     equip / unequip an item (item, 1) (the
//                             actor's EquipInventoryItem/UnequipInventoryItem script binds)
//     + 288                   its equipment manager; the worn clothing is the C_Item* vector at
//                             +64 (begin) / +72 (end), filled by the equip; the
//                             equipped weapons are the C_Item* slots at +152 (melee weapon),
//                             +160 (shield), +168 (ranged weapon), +176 (ammunition) and up to
//                             +208
//   item + 72                 the item's class; its class id (a GUID) at +8
//
// Engine thread only.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace kcds::runtime::appearance::outfit {

using Logger = void (*)(const char* format, ...);

// The item natives the book uses too (Client/runtime/book/resource_book.hpp).
struct Natives {
  void* (*services)() = nullptr;  // the game services getter
  void* inventory_find = nullptr;
  void* item_desc_init = nullptr;
  void* item_desc_free = nullptr;
  void* inventory_add = nullptr;
  void* inventory_remove = nullptr;
  void* item_health_set = nullptr;  // (item, health, change) clamped to the quality band
  // A weapon put on as the inventory screen's Equip does, ready in a weapon set (the inventory's
  // quickslots equip_weapon): 1 equipped, 0 refused, -1 not a weapon.
  int (*equip_weapon)(void* actor, void* item) = nullptr;
};

void install(const Natives& natives, Logger logger);

// Hooks the player's visor update so it skips a player without visor state. Putting
// a helmet with a visor on or off a player (the item listener) updates the visor toggle
// of C_Player + 3272; a player replica has none, and the game's call read through the null
// pointer when a replica took such a helmet off. False when the hook could not be installed.
bool install_visor_guard(void* visor_update);

// Whether the player's helmet has its visor down: it wears a helmet with a visor
// (what the visor update asks first: then gives the visor's id, 0 for none)
// and the player's vtable slot 184 says the visor is closed (as the update and the helmet's
// equip, read it). The two calls are taken from the visor update's own code. False
// for a player without the visor state (a replica) or before the guard is installed.
bool visor_closed(void* player);

// Hooks the check whether a soul's clothing preset is due so it never is for a
// replica this module dresses. A player's replica has an NPC soul, and the equipment of an NPC
// soul puts its clothing preset on again in its update once the preset is
// due, for example the frame after the soul's body parts changed; the preset takes off every
// piece it does not list. A replica wears its owner's outfit instead: without the hook
// a late joiner saw a player in armour stripped to one piece. False when the hook could not be
// installed.
bool install_preset_guard(void* preset_due);

// The outfit field of an actor's worn clothing and equipped weapons: "KO2" and per piece, in the
// order the game keeps them, its 16-byte class id, its quality (a byte) and its health (two
// bytes, little-endian, 0 .. 1 in 65535 steps). Empty when the actor has no soul. A replica's
// pieces are created with that quality and health (the attack a replica deals reads its
// weapon's health; replicas had quality 3 / 0.6 swords while their owners' had 0.1).
std::string capture(void* actor);

// Keeps the replica's worn clothing the one in outfit (a capture of its owner), every frame: a
// piece the owner does not wear comes off and out of the replica's inventory whenever it is on,
// the missing pieces are created and put on once per outfit. An empty outfit (not reported yet)
// leaves the replica as it is.
void apply(std::uint32_t player, void* actor, void* entity, const std::string& outfit);

// The items an actor wears and its equipped weapons (C_Item*), in the game's order; empty when
// it has no soul.
std::vector<void*> worn(void* actor);

// Puts an item of the actor's inventory on, or takes it off, through the game's own equipment
// (soul equipment slot 40 / 41). False when the game refused.
bool wear(void* actor, void* item, bool on);

// The pieces of an outfit field for the log: their count and the first part of each class id
// as the item tables write it ("pieces=2 01151dab 00b7ed62").
std::string describe(const std::string& outfit);

// The replica of player is gone.
void forget(std::uint32_t player);

// An item of the class created in the entity's inventory (a replica's), with the natives that
// create the outfit's missing pieces; nullptr when the entity has no inventory or the item could
// not be made.
void* create_item_in_inventory(void* entity, const std::array<std::uint8_t, 16>& class_id);

}  // namespace kcds::runtime::appearance::outfit
