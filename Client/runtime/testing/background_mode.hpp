// Test games in the background (tests, the KCDS splash included, must
// not take the focus or the controller, so the owner can play another game meanwhile).
//
// With KCDS_BACKGROUND=1 in the game's environment (the kit's and the studio's launch scripts
// set it; a player's own game never has it):
//   - no controller: the game's XInput reads (WHGame imports XInputGetState, XInputGetCapabilities
//     and XInputSetState from XINPUT9_1_0; it reads the pad every frame whether or not its window
//     is in front) answer ERROR_DEVICE_NOT_CONNECTED, and it never rumbles a pad;
//   - no focus: the game's SetForegroundWindow, SetActiveWindow and BringWindowToTop (WHGame
//     imports them) do nothing, ShowWindow shows without activating, SetWindowPos never activates
//     and never puts a window on top of every other;
//   - and should a window of this game come to the front anyway (the exe's splash dialog is shown
//     by user32 itself), the foreground goes straight back to the window that had it.
//
// A click on one of its windows with the mouse by hand (a button physically down over it) ends
// the mode for good: the game keeps the foreground and reads the controller as any game does, so
// the owner can take over a test game and play it.
#pragma once

namespace kcds::runtime::background {

using Logger = void (*)(const char* format, ...);

// Whether KCDS_BACKGROUND=1 is set for this game; always false in a release build (developer
// builds only, KCDS_DEV_BUILD).
bool enabled();

// Installs the hooks and the foreground watch when enabled(); MinHook must be initialized. Call
// before the game makes its first window (the exe's splash).
void install(Logger logger);

}  // namespace kcds::runtime::background
