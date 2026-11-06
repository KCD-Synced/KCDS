// The appearance of a body: the game's character components its
// soul is built from for body, head, hair and beard.
//
// A soul keeps its components as five names (body, head, hair, beard, underwear; one CryString
// each, in that order) in two sets (WHGame 1.5.5):
//
//   soul + 1072   the base set the game's rules gave it (Storm setBody/setHead/setHair/setBeard)
//   soul + 2784   a pointer to an override set (the barber's), allocated on first use; a part
//                 named there replaces the base one, an empty name hands it back to the base
//
// (soul, part, &name, 1) puts one name in the override set and rebuilds the soul's
// body; it is what the barber's TryHair and TryBeard do.
//
// The owner's client puts on what a resource asked for (Player.setAppearance) and publishes the
// chosen parts in the NetworkPlayer field "appearance" (for the resources) and the five names its
// body is built from with its gender in "bodyParts". A replica's soul gets base parts of its own
// (another face, other braies), so every other client puts all five of the owner's names in the
// replica's override set and gives it the owner's gender.
//
// The gender is the soul's archetype (soul + 3256, Libs/Tables/rpg/soul_archetype.xml): a female
// body gets the female counterpart of its archetype (Hero_female for the player's Hero,
// NPC_Female for a replica's NPC), which also brings that archetype's base armour, visibility
// and unarmed attack, and is built from the female tree; the base set is the male player's, so
// a female body names all five parts (female defaults where nothing was chosen).
//
// Engine thread only.
#pragma once

#include <cstdint>
#include <string>

#include "kcd2s/integration/appearance.hpp"

namespace kcds::runtime::appearance::components {

using Logger = void (*)(const char* format, ...);
using StringAssign = void*(__fastcall*)(void*, const char*);

struct Natives {
  void* set_component = nullptr;
  void* string_init = nullptr;
  void* string_free = nullptr;
  StringAssign string_assign = nullptr;
};

void install(const Natives& natives, Logger logger);

// What the actor's soul wears: the override set's names (empty: the game's own part).
kcd2s::integration::Appearance worn(void* actor);

// The five components the actor's body is built from: the override set's name where there is one,
// the base set's otherwise. Empty when the actor has no soul.
kcd2s::integration::BodyParts built_from(void* actor);

// Puts the named parts of a resource's request on the local player. False when the request
// cannot be put on (a female appearance).
bool put_on_request(void* actor, const kcd2s::integration::AppearanceRequest& request);

// Dresses the inventory paper-doll's actor in the whole appearance: its gender and its five parts
// (an empty part takes the gender's default) in the set the paper-doll is built from. False when
// the actor has no soul or the gender is unknown.
bool dress_dummy(void* actor, const kcd2s::integration::Appearance& appearance);

// The paper-doll figure (the pause screen's character stage) is built on its own base set, the
// male player's: while the look the game copied into it is female, the base set is made female too,
// so no male hands, feet, arms or legs show on it; a male look gets the base back.
// Engine thread, every frame the figure exists.
void keep_doll_base(void* actor);

// Builds the replica of player from the published "bodyParts" field; only a changed field or a new
// replica actor does anything. An empty field (not reported yet) leaves the replica as it is.
void show(std::uint32_t player, void* actor, const std::string& field);

// The replica of player is gone.
void forget(std::uint32_t player);

}  // namespace kcds::runtime::appearance::components
