// Dirt, blood and wear seen the same by everyone, WHGame 1.5.5.
//
// The game keeps how dirty and bloody a body and each piece of clothing or armour is in an
// armour runtime data (C_ArmorRuntimeData): the dirt at + 168 and the blood of the six body zones
// at + 169 .. + 174, each a byte of 0 .. 255 (a fraction of 255). A worn item's is its item + 88
// (the item state getter, null for an item that is not clothing); the body's is the
// actor's dirt helper (actor + 608, C_ActorDirtHelper) + 24. Washing at a trough or in a bath
// cleans them in the owner's game.
//
// The owner reports them in the replicated NetworkPlayer field "grime" (capture): the body's and,
// per worn piece, its class, its dirt, its blood and its wear (the item's health). Every other
// client puts them on the replica's own pieces of the same class (apply), the way the game loads
// them from a save (the bytes, then + 176 |= 3, + 180 = 1 and its change notice with
// the full-state key), so the renderer shows them.
//
// Engine thread only.
#pragma once

#include <cstdint>
#include <string>

#include "kcd2s/integration/grime.hpp"

namespace kcds::runtime::appearance::grime {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* item_state = nullptr;  // item -> its armour runtime data, or null
  void* changed = nullptr;     // (data, key) the data's change notice
  void* full_state = nullptr;  // qword_185324CF8: the key of a change of the whole state
};

// False when a native is missing.
bool install(const Natives& natives, Logger logger);

// The grime field of an actor: "KG1", the body's 7 bytes (dirt, blood of six zones; zeros when
// it has none), then per worn piece with clothing state its 16-byte class id, its 7 bytes and
// its wear (health 0 .. 1 as a byte). Empty when the actor has no soul.
std::string capture(void* actor);

// Puts a capture of the owner on its replica: the body's state and, for each piece the replica
// wears, the dirt and blood of the owner's piece of the same class (its health comes with the
// outfit, outfit.hpp). Applies only when the field or the replica's pieces changed since the last
// time. An empty field leaves the replica as it is.
void apply(std::uint32_t player, void* actor, const std::string& grime);

// A server's order on the local player (GRIME_ORDER): its body and/or worn clothes cleaned, or
// soiled with dirt and blood (every zone), through the same states; the owner's next capture
// shows it to everyone.
void order(void* actor, const kcd2s::integration::GrimeOrder& order);

// How dirty a horse is (Horse.dirt): its body's dirt and that of every piece of
// tack it wears set to `dirt` (0 to 1), blood left as it is; a horse's coat is drawn with the
// clothing system as a person's clothes are. Returns whether its body took it.
bool set_dirt(void* actor, float dirt);

// The replica of player is gone.
void forget(std::uint32_t player);

}  // namespace kcds::runtime::appearance::grime
