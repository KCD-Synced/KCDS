// The level loading screen's last seconds.
// The game draws its loading screen's spinner and hint
// (our "KCD2-Synced - Developed by KCDS Team", runtime_branding.cpp) on its loading thread. On a
// level load inside a world, C_UILoadingScreen's listener stops that thread on
// event 26, then event 28 (the level is loaded) starts its texture preload (state 2,
// TexturePreload.xml) without it, until event 33 ends the screen: about 4 seconds of a
// still picture with no spinner and no hint. After event 28 the thread is started again with the
// game's own start (which checks the multithreaded-render gate and its flag at + 152);
// event 33 stops it as before. The game itself starts the thread in that state on
// event 22. KCDS_LOADING_TAIL=0 turns it off.
#pragma once

namespace kcds::runtime::branding::loading_tail {

using Logger = void (*)(const char* format, ...);

// Hooks the loading screen's listener; false when the game's functions were not found.
bool install(Logger logger);

// Whether the game's level loading screen is drawn whole since the start (from event 28, the level
// loaded: before it, a load from the menu shows its picture's first, zoomed frame only); true also
// when the listener could not be hooked (nothing to wait for).
bool level_screen_shown();

}  // namespace kcds::runtime::branding::loading_tail
