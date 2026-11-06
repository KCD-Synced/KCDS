// World Architect's cursor: where the mouse is on the editor's stage and what it
// points at in the world, worked out by the game client every frame and sent to the editor, so
// the editor's preview, brush ring and target follow the cursor without asking the game
// (each question waits for a game frame and held the editor behind the mouse).
//
//   runtime -> game:  { op: 'architectCursor', on }      start or stop following the mouse
//   game -> runtime:  { op: 'architectCursorAt', x, y, over, hit }
//                     x, y the cursor on the 1920 x 1080 stage; over whether it is over the game
//                     window; hit a localWorldRay hit (physics/local_world.hpp) or null. Sent when
//                     the cursor or the camera moved, at most once a frame.
//
// The ray goes from the view camera's eye through the cursor (the camera fills the window), 500 m.
// Local mode only (local_mode.hpp). handle_runtime_message: any thread; frame: engine thread;
// pump: network thread.
#pragma once

#include <functional>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::architect::cursor_pick {

// True when the message was an architectCursor one.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Every frame in a world: the cursor and its hit, when they changed.
void frame(bool in_world);

// Sends the newest cursor message, if any.
void pump(const std::function<bool(const kcd2s::sdk::Value&)>& send);

}  // namespace kcds::runtime::architect::cursor_pick
