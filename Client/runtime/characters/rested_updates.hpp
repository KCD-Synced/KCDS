// A remote player's body skips the engine's own per-frame update while it rests. The
// body stays a full character; only how often that update runs changes. The engine sends event 33
// to every character's components and to its C_Actor on its worker threads (WHGame 1.5.5),
// which is most of what those workers do with a crowd in view.
#pragma once

namespace kcds::game_client {

// False when a function was not found: every body then keeps its update.
bool install_rested_updates();

// Once a frame on the engine thread, before the bodies: last frame's marks take effect.
void begin_rested_frame();
// On the engine thread: this entity skips the update next frame.
void mark_rested(void* entity);
// On the engine thread: this calm entity skips its combat update next frame (WHGame 1.5.5).
// Its target comes from its owner, and out of a fight it has none to keep.
void mark_calm(void* entity);

}  // namespace kcds::game_client
