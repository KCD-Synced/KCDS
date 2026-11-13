// What the local player's game puts in their hands for its own actions (the lockpick in
// a lock, the shovel while digging, the mug while drinking), so other players see it too.
//
// The game hands a tool to a body through its hand slots' holder (IActor slot 194):
// (holder, entity, slot 0 right / 1 left, attachment name, joint or null, flag) takes an
// entity into a hand,(holder, slot, attachment name, keep, flag) lets it go (as
// crafting/smithing.hpp uses them). Both are hooked: for the local player's own holder the held
// entity of each hand is kept, and its mesh is published with the player's animation props
// (kcd2s/integration/animation_props.hpp, the "animationProps" field), placed on the hand's joint
// where the owner's game holds it, so every other client shows the same mesh in that hand.
//
// A hand held by the station sync (smithing, alchemy) is the station's to show and is left out.
//
// Some of the game's minigames hang their tool on the hand without the hand slots: the grave
// digging (C_HoleDigging) spawns "player_shovel..." (a PickableItem named after the player) and
// binds it to the hand itself. Such a tool, an item named "player_..." within reach
// of a hand that holds nothing from the slots, is published the same way; the items are looked
// for twice a second. The lockpicking spawns two such items: "player_lockpick..." and "blade...",
// both with models under keys_lockpicks/, which is how the second is known.
//
// Engine thread (the game calls the hooks there).
#pragma once

#include <cstdint>
#include <vector>

#include "kcd2s/integration/player_animation.hpp"

namespace kcds::runtime::animation::hand_props {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* hand_take = nullptr;     // hooked
  void* hand_release = nullptr;  // hooked
  // The local player's hand slots' holder (IActor slot 194 of the local actor), or null.
  void* (*local_hands)() = nullptr;
  // The local player's body, or null.
  void* (*local_entity)() = nullptr;
};

// Hooks the take and the release; false when one is missing.
bool install(const Natives& natives, Logger logger);

// The props of the game's own hand tools this frame, as the field carries them: a mesh per hand
// with its offset from the hand's joint (engine thread).
std::vector<kcd2s::integration::AnimationProp> props();

// The entity the local player's game holds in a hand slot (0 right, 1 left), 0 for none (engine
// thread).
std::uint32_t held_entity(std::size_t hand);

}  // namespace kcds::runtime::animation::hand_props
