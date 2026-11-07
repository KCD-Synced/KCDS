// World Architect's materials and textures made in the editor (Client/resources/architect/
// materials.cjs): what a model's material slots are, a material's text to build on, and the files
// the editor makes, written into the author's assets folder (user_assets.hpp mounts them).
//
//   sync localModelSlots { mesh }  -> { material, slots: [name] } from the model's material chunk
//                                     (0x1014: its material, then each sub-material's name), the
//                                     model read from the assets folder (objects/kcds/...) or the
//                                     game's files; null when it cannot be read
//   sync localAssetText { path }   -> a material's (.mtl) text from the assets folder or the
//                                     game's files (up to 1 MiB), or null
//   sync localAssetBytes { path, offset, length }  -> { size, data } a part of a model (.cgf, up
//                                     to 8 MiB) from the assets folder or the game's files, at
//                                     most 36 KiB from offset as base64 (the last model asked is
//                                     kept, so its parts are not read again); null when unread
//   sync architectSplitModel { from, to, material }  -> { meshes: [path] } a building's streamed
//                                     meshes (from, a .cgfm) as models of their own at
//                                     <to>_<n>.cgf drawn with material (model_split.hpp), for an
//                                     opened building; null when it cannot be read or written
//   architectAssetWrite { path, data, part, parts }   a file of the editor's (base64 data, in
//                                     parts of a message each, in order from 0), written once the
//                                     last came, at objects/kcds/<folder>/... in the assets folder:
//                                     a .mtl or .dds of at most 32 MiB, or a .cgf under
//                                     objects/kcds/opened/
//
// Local mode only (local_mode.hpp). handle_runtime_message: any thread; pump: network thread.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::architect::asset_files {

// True when the message was one of the above (a sync question is answered at the next pump).
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Sends the answers made.
void pump(const std::function<bool(const kcd2s::sdk::Value&)>& send);

}  // namespace kcds::runtime::architect::asset_files
