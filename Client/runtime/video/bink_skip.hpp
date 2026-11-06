// The game's videos never skip frames to catch up.
//
// The render thread decodes the playing video (the main menu's backdrop behind the KCDS menu, a
// loading video) with Bink. When a frame comes late, the game catches up in a loop (WHGame):
// while BinkShouldSkip, BinkNextFrame, BinkDoFrameAsyncMulti and
// BinkDoFrameAsyncWait with no time limit. That wait can stay without an answer: no Bink worker
// takes the frame, the render thread waits forever and the main thread with it (its frame waits
// for the render thread), so the game stood on "Entering the world" with the loading
// screen opened and the level never loading. Two games starting at once, or a slow machine,
// made the menu video late. BinkShouldSkip answers "no": a late video shows its next frame
// instead of skipping to the right one, and the catch-up loop is never entered.
#ifndef KCDS_RUNTIME_VIDEO_BINK_SKIP_HPP
#define KCDS_RUNTIME_VIDEO_BINK_SKIP_HPP

namespace kcds::runtime::video::bink_skip {

using Logger = void (*)(const char* format, ...);

// Call once the game module is loaded (bink2w64.dll comes with it), after MH_Initialize and
// before the main menu plays its video. Returns false when the hook could not be installed.
bool install(Logger logger);

}  // namespace kcds::runtime::video::bink_skip

#endif  // KCDS_RUNTIME_VIDEO_BINK_SKIP_HPP
