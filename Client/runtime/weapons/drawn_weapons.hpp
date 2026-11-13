// A weapon a player draws is drawn on its replica too.
//
// A human's weapon controller (actor vtable slot 191, WHGame 1.5.5) keeps its weapon sets:
//
//   controller + 64   the set in use
//   controller + 72   the sets, two item WUIDs each (primary, secondary)
//
// human:DrawWeapon puts the set's two items in the hands, each with
// (controller, item WUID, 2); human:HolsterWeapon puts them back,
// each (controller, item WUID). What a human holds is in its item holders
// (actor + 2448): the item WUID in the right hand at + 608, in the left at + 616. Items are found
// by WUID in the item manager (* + 360): item + 48 is its WUID, + 72 its
// class, whose id (a GUID) is at + 8.
//
// The owner's client publishes the classes of the items in its player's hands in the
// NetworkPlayer field "hands", with owner-generated item lifetime tokens
// (kcd2s/integration/hands.hpp). Every other client puts the
// replica's items of those classes, from the replica's weapon sets (the outfit sync equips the
// owner's weapons on the replica), in the replica's hands and holsters what the owner does not
// hold. An item the owner drew from outside its weapon sets (a quickslot) is not in the replica's
// sets: the replica gets one of that class in its inventory, once, and draws it the same way. It
// does so when the field or the replica's weapon sets change, so a client that joins later sees the
// weapons drawn too, and when the replica's game takes an item out of its hand while the owner
// still holds it (a replica hit by an arrow lets its drawn bow go).
// Combat resolves a token only while the currently held native item still matches its
// validated binding. No native item UID or allocation address is sent to another process.
//
// Engine thread only.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace kcds::runtime::weapons {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* draw_item = nullptr;
  void* holster_item = nullptr;
  void* find_item = nullptr;
  void* const* item_manager = nullptr;
  // An item of a class made in an entity's inventory (appearance::outfit's).
  void* (*create_item)(void* entity, const std::array<std::uint8_t, 16>& class_id) = nullptr;
  // What an actor wears, its equipped weapons included (appearance::outfit's).
  std::vector<void*> (*worn)(void* actor) = nullptr;
};

// False when a native is missing.
bool install(const Natives& natives, Logger logger);

// The "hands" field of an actor: the classes of the items in its hands.
std::string capture(void* actor);

// Current item lifetime identity, scoped to actor. Zero is None; unknown, stale,
// dropped or mismatched nonzero items fail closed. No native UID travels on the wire.
std::optional<std::uint32_t> item_token(void* actor, std::uint64_t native_uid);
std::optional<std::uint64_t> item_from_token(void* actor, std::uint32_t token);

// Updates current combat identity bindings without drawing, holstering or changing animations.
// Observe the latest field even while native rider/carry actions own the replica's presentation.
void observe_replica_items(std::uint32_t player, void* actor, const std::string& field);

// The item in an actor's left or right hand, or null.
void* item_in_hand(void* actor, bool left);

// The item class (its GUID) of what an actor holds in its left or right hand; false for an empty
// hand (issue 016: what an NPC's brain put in its hands).
bool class_in_hand(void* actor, bool left, std::array<std::uint8_t, 16>& item_class);

// The WUID of the item an actor strikes with: the one in its right hand, else in its left, else 0
// (unarmed). A relayed HitInfo travels without it (combat.hpp portable_reaction_data), and the
// game picks the blow's impact effect and sound from it.
std::uint64_t striking_item(void* actor);

// Draws the first item of the actor's weapon sets that `pick` accepts (an NPC replica's bow,
// the replica's set draw put nothing in its hands), as human:DrawWeapon draws a set's
// item. False when none is accepted.
bool draw_set_item(void* actor, bool (*pick)(void* item));

// Puts what an actor holds in its hands away, as human:HolsterWeapon does (for each
// hand's item). False when it held nothing.
bool holster(void* actor);

// Keeps the replica of player (its actor and engine entity) holding what its owner's field names.
void keep_replica(std::uint32_t player, void* actor, void* entity, const std::string& field);

// The replica of player is gone.
void forget(std::uint32_t player);

// The replicas of this world go.
void reset();

}  // namespace kcds::runtime::weapons
