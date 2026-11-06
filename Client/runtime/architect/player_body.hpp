// World Architect's player body: while the editor is open, the author's own character
// is hidden (the free camera starts where it stands, and it is no part of the work); Play as
// player and Exit show it again.
//
//   { op: 'architectPlayer', hidden }
//
// The body is hidden as the entity binding's Hide does (CEntity slot 63, props/prop_entity.hpp).
// Local mode only (local_mode.hpp). handle_runtime_message: any thread; frame: engine thread.
#pragma once

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::architect::player_body {

// True when the message was an architectPlayer one.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Whether the editor is open (it hid the body). Any thread.
bool editor_open();

// Every frame with the local player's entity: hidden while the editor wants it so, shown once
// when it no longer does.
void frame(void* local_entity);

}  // namespace kcds::runtime::architect::player_body
