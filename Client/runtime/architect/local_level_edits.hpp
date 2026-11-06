// World Architect's edits of the level's own objects: a tree, a bush, a brush or a
// level entity the author picked, hidden in this game and shown again. Only in the local mode
// (local_mode.hpp); nothing is replicated. Applied by the same code as the server's level edits
// (level/level_edits.hpp), with ids of their own.
//
//   { op: 'localLevelEdit', kind: 'set', id, object: 'brush' | 'entity', name, guid, at, hidden,
//     position?, rotation?, scale? }
//   { op: 'localLevelEdit', kind: 'remove', id }
//
// name: a brush's mesh path; guid: an entity's EntityGuid as sixteen hex digits; at: where the
// level put it; position, rotation (x, y, z, w) and scale (0.1 to 10): where it stands instead
// (moved where it is, as a server's level edit moves it). Requests: any thread
// (handle_runtime_message). take(): engine thread.
#pragma once

#include <vector>

#include "kcd2s/integration/level_edits.hpp"
#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::architect::level_edits {

// True when the message was a localLevelEdit one (queued in the local mode, dropped otherwise).
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// The changes asked for since the last call, oldest first.
std::vector<kcd2s::integration::LevelEditChange> take();

}  // namespace kcds::runtime::architect::level_edits
