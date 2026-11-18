// Your own skinned meshes on the studio character: a .skin made by
// Dev/tools/assets/blender/kcds_skin.py (bound to the character's own skeleton) and its .mtl are
// read from %LOCALAPPDATA%\KCDS\studio\meshes\<name>.skin, put in the game's file system at
// objects/kcds_studio/<name>_<n>.skin in a pack from memory (native_ui mount_files), loaded by the
// game and worn by the character as a skin attachment, so it moves with the character's bones.
//
//   { op: 'studioSkin', name }   wears that skin (the one before goes); name '' takes it off
//
// The skin is worn the game's own way, as its clothing puts on a piece
// (appearance/skin_binding.hpp), on an attachment of the studio's own.
//
// handle_runtime_message: any thread; tick: engine thread.
#pragma once

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::studio::skin {

bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Every frame in the studio's world: puts on or takes off what the editor asked, on the
// character `local_entity`.
void tick(void* local_entity);

}  // namespace kcds::runtime::studio::skin
