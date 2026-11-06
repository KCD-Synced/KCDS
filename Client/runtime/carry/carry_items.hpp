// Items carried in the arms (Player.holdItem, dropHeldItem, heldItem), with the
// game's own carry, the action its sacks use (C_ActorActionCarryItem, WHGame 1.5.5).
//
// An actor's actions are run by its action actor (actor + 640, C_ActionActor): its vtable + 64
// finds the running action of a type (29 is the carry: actor, &action, type), + 32 starts one
// (actor, &action), the way the sacks' pick-up does (CarryableItemBind.PickUp, then).
// The carry action is made (the action actor's factory at + 0x50,
// &action, &params) from its parameters: the item's handle, an animation tag ("" for the item's
// own) and a flag. The action keeps the handle at + 80 and its phase at + 144: 0 new, 1 taking
// up, 2 restored, 3 carried, 4 putting down, 5 put down. A new action takes the item up from the
// ground with the game's clip (1), then carries it (3). (One in phase 2 when it starts, as a
// loaded game's is, attaches the item at once; the game refuses that for an item
// lying in the world, so a new carry here always takes its item up.) The game's put-down key
// runs (action): the put-down clip (4), whose Detach event lets the item go where it
// is (5) and whose end runs (action): it lets go if that has not happened,
// finishes the action and clears its clip. Calling at once lets go without the clip.
// Actions are reference counted (vtable + 8 adds one, + 16 releases one).
//
// A carried item is a game item lying in the world (the world's inventory) when the carry
// starts: a new one is laid at the actor (ground::lay) first. An item this module laid is taken
// away again once the carry ends (part 1: the put-down does not leave a prop yet).
// A replica carries what its owner carries the same way, and lets go at once when its owner's
// carry ends.
//
// Engine thread.
#ifndef KCDS_RUNTIME_CARRY_CARRY_ITEMS_HPP
#define KCDS_RUNTIME_CARRY_CARRY_ITEMS_HPP

#include <array>
#include <cstdint>
#include <string>

namespace kcds::runtime::carry {

using Logger = void (*)(const char* format, ...);
using Guid = std::array<std::uint8_t, 16>;

struct Natives {
  void* create_action = nullptr;  // (factory, &action, &params)
  void* put_down = nullptr;       // (action)
  void* let_go = nullptr;         // (action)
  void* find_item = nullptr;      // (registry, &handle)
  void* const* item_manager = nullptr;
  // Lays a new item of a class in the world at a point (ground::lay); the game's item.
  void* (*lay)(const Guid& item_class, const float* position) = nullptr;
  // Takes an item lying in the world away (ground::take_away).
  bool (*take_away)(void* item) = nullptr;
  // Whether an item lies in the world (ground::lies_in_world).
  bool (*lies_in_world)(void* item) = nullptr;
  // Where an item lying in the world is; false when it has no entity.
  bool (*item_position)(void* item, float* position) = nullptr;
  // Tells the server where the item of the local actor's carry was let go (CARRY_REPORT).
  void (*report_put_down)(const Guid& item_class, const float* position) = nullptr;
};

// False when a native is missing.
bool install(const Natives& natives, Logger logger);

// The class of the item the actor carries, from taking it up until it is put down; false when
// it carries none.
bool carried(void* actor, Guid& item_class);

// Has the local actor carry a new item of the class: it is laid at position (the actor's feet)
// and taken up with the game's clip. False when the game will not (the actor cannot act, already
// carries, or has no pick-up clip for the item).
bool carry_new(void* actor, const Guid& item_class, const float* position);

// Has the local actor take up an item lying in the world (a stack the server laid for everyone)
// with the game's clip and carry it. False when the game will not.
bool carry_laid(void* actor, void* item);

// Has the actor put the carried item down: with the game's put-down, or let go at once. False
// when it carries none.
bool put_down(void* actor, bool at_once);

// The "carry" field of the local actor (kcd2s/integration/carry.hpp). Once the carry of an item
// this module laid or took up is over, an item let go in the world is reported where it lies
// (the server lays a carryable prop there for everyone) and taken away; the field keeps naming
// the class for a moment after, so that the server hears where it lies first.
std::string capture(void* actor);

// Keeps the replica of player (its actor, and where it stands) carrying what its owner's field
// names.
void keep_replica(std::uint32_t player, void* actor, const float* position,
                  const std::string& field);

// The replica of player is gone.
void forget(std::uint32_t player);

// The replicas of this world go.
void reset();

}  // namespace kcds::runtime::carry

#endif  // KCDS_RUNTIME_CARRY_CARRY_ITEMS_HPP
