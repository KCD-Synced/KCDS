// Movement keys let go when the game window loses the focus (after an
// Alt+Tab on a ladder the player kept climbing, stuck in the climb).
//
// A single-player game pauses when its window loses the focus; in a server world the client
// refuses that pause (hooked_pause_game), so the world runs on while the window is in the
// background, and a key held at the switch stays held: its release goes to the other window. The
// movement actions the player pressed while the game was in the foreground are noted, and when the
// game's process loses the foreground they are released through the player's input, the path a key
// release takes. Presses made while the game is in the background (the kit's scripted input) are
// left alone.
//
// Engine thread.
#ifndef KCDS_RUNTIME_INPUT_FOCUS_RELEASE_HPP
#define KCDS_RUNTIME_INPUT_FOCUS_RELEASE_HPP

namespace kcds::runtime::input::focus {

using Logger = void (*)(const char* format, ...);

// The player input's OnAction: (the player input, the action id, mode, value).
using Deliver = void*(__fastcall*)(void* input, const char** action, unsigned mode, float value);

// The local player's input took an action (mode 1 press, 2 release).
void on_action(void* input, const char** action, unsigned mode);

// Every frame: releases the noted movement keys once the game has lost the foreground.
void tick(Deliver deliver, Logger logger);

}  // namespace kcds::runtime::input::focus

#endif  // KCDS_RUNTIME_INPUT_FOCUS_RELEASE_HPP
