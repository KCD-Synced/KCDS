// The props a player's script animation holds in the player's hands
// (Player.playAnimation `props`), on every client: the owner's own body and every replica show what
// the player's NetworkPlayer field "animationProps" names (kcd2s/integration/animation_props.hpp).
//
// A prop is a BasicEntity of its own showing the mesh, without physics (props/prop_entity.hpp, as
// the held documents of): a prop-catalog mesh by its path, or an item class's own model.
// The class is the game's (inventory::game_item_class); its "Model" (+ 0x38) and "Material"
// (+ 0x40) are CryStrings (the RTTR registration of wh::entitymodule::PickableItem, WHGame 1.5.5),
// relative to Objects/, a model without an extension being a .cgf (as item.xml's
// 1759 models lie in the paks). A .cgf goes in with LoadGeometry, a .cdf or .chr with
// LoadCharacter. It hangs from the joint of the body's character by name (CEntity::AttachChild,
// flag 4): RightHand or LeftHand unless the request names one, at the request's offset
// and rotation local to the joint (none: on the joint, unturned). It is attached on a frame after
// its spawn: the entity system finishes a spawn at the end of its frame (the held documents wait
// the same way).
//
// A prop that follows the animation waits out of the hand until the animation itself
// takes it: CSkeletonAnim's own slot 46 hands every event of a clip to its character,
// and a pickup, spawnitem or attach event for that hand (or one naming no hand) puts the prop in
// it, a place, despawnitem or detach takes it out. Every client sees the events of its own copy of
// the body, so each shows it at the same moments of the animation.
//
// Engine thread only (the event hook only queues the events, under a lock).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "kcd2s/integration/guid.hpp"

namespace kcds::runtime::animation_props {

struct Natives {
  void* attach_child = nullptr;  // CEntity::AttachChild
  // The game's own class of an item class GUID (inventory::game_item_class), or null.
  void* (*item_class)(const kcd2s::integration::Guid& id) = nullptr;
  void (*log)(const char* format, ...) = nullptr;
  // The game's image, for the skeleton's vtable (the animation events).
  std::uint8_t* image = nullptr;
  std::size_t image_size = 0;
};

void install(const Natives& natives);

// The mesh file (under Objects/, a .cgf when the class names none) and material of an item
// class's own model; false for a class this game does not have or one without a model.
bool item_mesh(const kcd2s::integration::Guid& item, std::string& file, std::string& material);

// The local player's own body, as a holder.
inline constexpr std::uint32_t kLocalHolder = 0;

// Every frame: the holder's body (its engine entity) shows what the field names; a new body or a
// new field puts the old props away first. holder: kLocalHolder or a replica's network id.
void keep(std::uint32_t holder, void* body, const std::string& field);

// The holder's body is gone: its props go too.
void forget(std::uint32_t holder);

// The world was reset: the props went with it.
void reset();

// The moments the local player's animation took a prop that follows it into the hand (taken) or
// put it down, since the last call, oldest first: the prop's index in the field.
struct OwnCue {
  std::uint8_t prop = 0;
  bool taken = false;
};
std::vector<OwnCue> take_own_cues();

}  // namespace kcds::runtime::animation_props
