// Drawing a player's body as a prop mesh (Player.setDisguise).
//
// The mesh is loaded into a free slot of the player's entity and the character slot (0), with
// everything the character wears, is not drawn. The mesh slot sits at the entity's origin, so it
// stands where the body stands and turns with it (CEntity, WHGame 1.5.5):
//
//   vtable slot 108      LoadGeometry(entity, slot, file, geometry name, flags), slot -1 takes a
//                        free one; returns the slot or a negative value
//   vtable slot 87       FreeSlot(entity, slot)
//   vtable slot 98 / 99  SetSlotFlags / GetSlotFlags; bits 1 and 2 draw the
//                        slot, as the entity script binding's DrawSlot sets them
//
// The body's collision cylinder (the living entity's player dimensions) is resized around the
// mesh: its radius and height from the box of what the entity draws now (the render component's
// local bounds), and kept so while the game sets it again on a change of stance; taking the mesh
// off gives the game's newest collider back.
//
//   vtable slot 79                    GetPhysics
// (entity, 0)             the entity's render component; its vtable slot 18 is
//                                     GetLocalBounds (as GetLocalBBox calls it)
//   CLivingEntity slot 4 / 5          SetParams / GetParams of pe_player_dimensions, type 1
//                                     (checks it,reads it)
//
// The owner's client wears what a resource asked for (DISGUISE) and publishes the mesh it wears
// in the NetworkPlayer field "disguise"; every other client draws the replica the same. A mesh
// this game cannot load is not worn, so "disguise" reads "".
//
// Engine thread only.
#pragma once

#include <cstdint>
#include <string>

namespace kcds::runtime::appearance::disguise {

using Logger = void (*)(const char* format, ...);
// an entity's component by its id.
using ComponentLookup = void*(__fastcall*)(void* entity, unsigned id);

void install(ComponentLookup component, Logger logger);

// Keeps the local player's body drawn as model ("" for itself); returns the mesh it wears.
std::string keep_local(void* entity, const std::string& model);

// Keeps the replica of player drawn as model ("" for itself).
void keep_replica(std::uint32_t player, void* entity, const std::string& model);

// The replica of player is gone.
void forget(std::uint32_t player);

}  // namespace kcds::runtime::appearance::disguise
