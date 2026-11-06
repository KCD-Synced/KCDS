// World Architect's own assets:
// models, materials and textures of the author's own, put in
// Documents\KCDS\World Architect\assets at their game paths, the same layout as a resource's
// stream/ folder (<top folder>/kcds/<name>/...), so the same files later ship with a server's
// resource unchanged. The game client mounts them as a resource's streamed files are mounted
// (native_ui::mount_local_files) and tells the editor what there is:
//
//   sync localUserAssets {}  -> { folder, meshes: [path], materials: [path without .mtl],
//                                 files, refused: [path] }
//
// Each question looks at the folder again; files that changed are mounted again (a pack of a new
// name). A file outside <top>/kcds/<name>/, of another kind (.cgf, .cgfm, .mtl, .dds and its
// .dds.N parts, .chr, .skin, .cdf) or too large is refused and named. Local mode only.
// handle_runtime_message: any thread; frame: engine thread; pump: network thread.
#pragma once

#include <functional>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::architect::user_assets {

// True when the message was a localUserAssets question.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Answers the questions asked since the last frame (looking at the folder, mounting it).
void frame(bool in_world);

// Sends the answers made.
void pump(const std::function<bool(const kcd2s::sdk::Value&)>& send);

}  // namespace kcds::runtime::architect::user_assets
