// The game's lockpicking minigame (C_LockPicking), so it can end before the world does: a player
// who left for the main menu in the middle of it kept the lock's screen over the menu, because only
// the minigame's end takes its view (C_LockPickingView, "uilockpicking") away.
//
// The minigame's frame update is hooked to know the running one. Its end is its vtable
// slot 4, as its fail step calls it with no lockpick left: no unlock and no reward, which
// would give.
//
// Engine thread.
#pragma once

namespace kcds::runtime::lockpicking {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* update = nullptr;  // hooked
};

// Hooks the update; false when it is missing.
bool install(const Natives& natives, Logger logger);

// Leaving the world: true while the running minigame still ends and the caller should wait. Ends it
// the first time; gives up after a few seconds, when a paused game does not run the end.
bool ending_for_leave();

}  // namespace kcds::runtime::lockpicking
