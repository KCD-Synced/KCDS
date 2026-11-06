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
// While it is on, nothing of this game can be brought to the front by hand either: it is for
// games no one plays.
#pragma once

namespace kcds::runtime::background {

using Logger = void (*)(const char* format, ...);

// Whether KCDS_BACKGROUND=1 is set for this game.
bool enabled();

// Installs the hooks and the foreground watch when enabled(); MinHook must be initialized. Call
// before the game makes its first window (the exe's splash).
void install(Logger logger);

}  // namespace kcds::runtime::background
