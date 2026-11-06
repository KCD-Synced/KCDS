// The local player's state, captured and sent every frame: its pose, motion,
// animations, look, aim, appearance, carried things, stats and the rest of the fields every
// other client shows its replica from.
#pragma once

#include "../core/frame.hpp"

namespace kcds::game_client {

// Called by frame() every frame once the player has a network id. Sets sent when the state went
// to the server; otherwise the local pose was not readable this frame, and frame() returns at once.
void send_local_state(const Frame& frame, bool& sent);

}  // namespace kcds::game_client
