// Your own skinned meshes on the studio character: a .skin made by
// Dev/tools/assets/blender/kcds_skin.py (bound to the character's own skeleton) and its .mtl are
// read from %LOCALAPPDATA%\KCDS\studio\meshes\<name>.skin, put in the game's file system at
// objects/kcds_studio/<name>_<n>.skin in a pack from memory (native_ui mount_files), loaded by the
// game and worn by the character as a skin attachment, so it moves with the character's bones.
//
//   { op: 'studioSkin', name }   wears that skin (the one before goes); name '' takes it off
//
// The game's own way, as its clothing puts on a piece: the
// character manager's LoadModelSKIN (ICharacterManager slot 6), a CSKINAttachment (64
// bytes of the game's allocator, constructor) holding the attachment's skin interface
// (IAttachment slot 29) with a reference, and IAttachment AddBinding (slot 26: object, skin,
// flags 2048). The attachment is made once with IAttachmentManager CreateAttachment (slot 2,
// name and type 2 for a skin, as the attachment list loader does) and found again by
// name (slot 8); taking a skin off is its ClearBinding (slot 30).
//
// handle_runtime_message: any thread; tick: engine thread.
#pragma once

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::studio::skin {

struct Natives {
  void* const* character_manager = nullptr;  // qword_185189BE0
  void* allocate = nullptr;                  // the game's allocator: memory of `size` bytes
  void* skin_attachment = nullptr;           // CSKINAttachment constructor
};

void install(const Natives& natives);

bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Every frame in the studio's world: puts on or takes off what the editor asked, on the
// character `local_entity`.
void tick(void* local_entity);

}  // namespace kcds::runtime::studio::skin
