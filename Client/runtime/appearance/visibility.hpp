// Whether a player's body is drawn (Player.setVisible).
//
// An engine entity that is invisible is not drawn; it keeps its physics, its soul and everything
// else (CEntity, WHGame 1.5.5):
//
//   entity + 8      flags; 0x40000 is invisible
//   vtable slot 65  Invisible(entity, invisible, recursive) sets the flag and sends the entity its
//                   event
//   vtable slot 66  IsInvisible
//   entity + 152    its children (a pointer to a vector of CEntity*): what is attached to it
//
// A hidden body is kept hidden with all its children every frame, so what is attached later (a
// held prop) is hidden too. Showing it again makes visible only the entities this module hid that
// are still part of the body; what the game itself made invisible stays so.
//
// The owner's client hides its own body when a resource asks (BODY_VISIBLE) and publishes it in
// the NetworkPlayer field "bodyVisible"; every other client hides the player's replica the same.
//
// Engine thread only.
#pragma once

#include <cstdint>

namespace kcds::runtime::appearance::visibility {

// Keeps the local player's body drawn or not.
void keep_local(void* entity, bool visible);

// Keeps the replica of player drawn or not.
void keep_replica(std::uint32_t player, void* entity, bool visible);

// The replica of player is gone.
void forget(std::uint32_t player);

}  // namespace kcds::runtime::appearance::visibility
