// Snow stays out from under every roof. The renderer draws a top-down depth map of the level's
// "rain occluders" around the camera, and the snow shader (DeferredSnow, $RainOcclusion) and the
// falling flakes leave out what lies under them. The game fills that map only while it rains, and
// KCD2's levels mark no object as a rain occluder, so with snow it stayed empty. While snow is
// on, the map's update draws the level's buildings around the camera instead: their meshes and
// world matrices are handed to the update for its call (the game's own list is put back after),
// and its rain check answers "not skipped" for that call only (the rain passes keep theirs).
#pragma once

namespace kcds::runtime::winter::roof_occluders {

using Logger = void (*)(const char* format, ...);

// `gate`: the renderer's rain occlusion update, whose first call (+ 23) is its rain check, which
// loads the renderer at + 36 (mov rcx, [rip]) and the per-thread occluder lists at + 43
// (lea rbp, [rip]).
bool install(void* gate, Logger log);

// Main thread: the level's buildings around the camera become the map's occluders (refreshed
// twice a second while snow is on).
void update(const float camera[3]);

// Whether the map holds the level's buildings now (installed, snow on, buildings around).
bool occluding();

}  // namespace kcds::runtime::winter::roof_occluders
