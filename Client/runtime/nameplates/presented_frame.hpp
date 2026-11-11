// Which frame the render thread is drawing, so the nameplates painted on it are the ones worked out
// for it (WHGame 1.5.5). The engine's sync between the engine and render threads (after the engine
// waits for the render thread to finish its frame) writes the frame number it hands over at
// renderer + 0x29C0 (the global holding the renderer is read by `mov rcx,[rip+...]` there), then
// wakes the render thread, which draws and presents that frame. Until the next sync the number
// stays: the engine thread, working out frame F, still reads F - 1 there; the render thread,
// presenting F, reads F.
//
// Engine thread: tag what is worked out this frame with next(). Render thread: current() is the
// frame being drawn.
#pragma once

#include <cstdint>

namespace kcds::runtime::nameplates::presented_frame {

// renderer: the global holding the renderer. Null leaves the frames unknown.
void install(void* const* renderer);

// The frame the render thread draws now; -1 while unknown.
std::int64_t current();

// The frame the engine thread works out now (current() + 1 there); -1 while unknown.
std::int64_t next();

}  // namespace kcds::runtime::nameplates::presented_frame
