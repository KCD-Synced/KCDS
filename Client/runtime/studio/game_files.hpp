// The game's own files for the studio's catalogue (Asset/Animation Studio): every object model
// (Objects/....cgf, its _lod models left out: 11,100 in 1.5.5) and every animation clip
// (Animations/....caf: 3,153) in the game's packs. The packs (Data/*.pak, zip files of at most
// 2 GB) are read once, only their central directories (the file names), on a thread of their
// own the first time the editor asks; then each search answers at once.
//
//   { op: 'studioGameFiles', kind: 'objects' | 'clips', query, page, size? }
//        -> { op: 'studioGameFilesPage', kind, query, page, size, pages, total, paths: [...] }
//           (`size` a page, 24 when not given, at most 200; paths in name order; `query` words
//           all found in the path, any case; `ready: false` while the packs are still read)
//
// handle_runtime_message: any thread; tick: engine thread.
#pragma once

#include <functional>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::studio::game_files {

bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Every frame in the studio's world: answers the searches (send: to the resource runtime).
void tick(const std::function<bool(const kcd2s::sdk::Value&)>& send);

}  // namespace kcds::runtime::studio::game_files
