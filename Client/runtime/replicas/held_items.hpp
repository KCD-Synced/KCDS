// What a replica holds or wears as an entity of its own (a torch or a weapon in a hand, a sword
// in its scabbard, a shield on its back), drawn with its skeleton.
//
// The game hangs such an entity on an attachment ("hand_left", "hand_right", the back slots)
// through a CEntityAttachment: the entity is moved to where the attachment is once the body's
// animation is done. On a replica that is one pose late: a walking replica's torch trailed its hand
// by up to a hand's breadth and jumped as it stopped, while what the skeleton draws itself (its
// clothes) never does. So the entity's mesh is also hung on the skeleton: an attachment of its own
// on the hand's joint, where the hand's attachment sits, holding the mesh as a CCGFAttachment (as
// the game's own attachment of a mesh by name does: the mesh's reference at + 8 of 24 bytes of the
// game's allocator, IAttachment AddBinding, slot 26), and the entity's own mesh slot is not drawn
// (its flame and light stay as they are). When the hand lets the entity go, the mesh goes and the
// entity draws its own again.
//
// Engine thread.
#pragma once

namespace kcds::game_client {

// Finds the game's functions; false when unavailable.
bool install_held_items();

// Once a frame: the replicas' attached item entities, drawn on their skeletons.
void tick_replica_held_items();

}  // namespace kcds::game_client
