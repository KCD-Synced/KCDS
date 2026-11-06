// The replicas' frame tick: the other players' replicas spawned through the
// engine's actor factory, updated from their owners' replicated state and removed when their
// owners leave.
#pragma once

#include "../core/frame.hpp"

namespace kcds::game_client {

// Before any player, NPC or horse factory runs this frame.
void retire_remote_players(const Frame& frame);

// Called by frame() every frame, after the local player's state was sent. Sets finished when it
// ran to its end; otherwise the adapter stopped (failed) or a replica's native rider state asked
// frame() to stop for this frame, and frame() returns at once.
void tick_replicas(const Frame& frame, bool& finished);

}  // namespace kcds::game_client
