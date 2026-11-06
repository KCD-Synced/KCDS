// Remote players shown smoothly: each one's owner as it was a moment ago on the
// owner's own clock, and the state shown this frame.
#pragma once

#include <unordered_map>

#include "../../../Backend/integration/local/runtime.hpp"
#include "kcd2s/integration/player_replica_timeline.hpp"

namespace kcds::game_client {

namespace core = kcd2s::core;

using RemoteState = kcd2s::replication::Mirror::Entity;
extern std::unordered_map<core::EntityId, kcd2s::integration::PlayerReplicaTimeline<RemoteState>>
    replica_timelines;
extern std::unordered_map<core::EntityId, RemoteState> replica_shown;
// A replicated position this far from the replica in one update is a teleport, not movement.
inline constexpr float kReplicaTeleportMeters = 5.0f;
const RemoteState& shown_remote_state(core::EntityId id, const RemoteState& latest, double now);

}  // namespace kcds::game_client
