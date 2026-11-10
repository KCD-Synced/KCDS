// A replica's routine move, applied just before the game's next update.
//
// The client's frame runs after the game's update (core/engine_loop.cpp). The update animates the
// bodies and places what hangs on them (a torch in a hand, the sword and bow a body wears) from
// where each body stands then. A replica moved in the frame was drawn at its new place with those
// things still at the old one: on a walking or turning player they trailed and jumped. Moved just
// before the update instead, the body and what it carries are placed together, a frame later.
#pragma once

#include <cstdint>

#include "../core/client_internal.hpp"

namespace kcds::game_client {

// Engine thread: moves the replica body `engine_id` to p, q before the next update (the last
// move asked for a body wins).
void defer_replica_move(std::uint32_t engine_id, const core::Vec3& p, const core::Quat& q);

// Engine thread, just before the game's update: the moves asked for since the last one.
void apply_replica_moves();

}  // namespace kcds::game_client
