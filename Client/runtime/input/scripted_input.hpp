// Scripted game input for live tests (KCDS_TEST_HOOKS only): the game's own input actions
// (moveforward, sprint, toggle_run, toggle_crouch, jump, use, ...) pressed and released on the
// local player as its keyboard would, so movement, pace and stance can be tested with nobody at
// the keyboard (injected key events never reach the game).
//
// The kit writes %TEMP%\kcds-input-<pid>.txt and signals Local\KCD2S.InputScript.<pid>. Each line
// is "<seconds> <action> <mode> <value>": that many seconds after the signal, the action goes to
// the player's input (C_PlayerInput, actor + 2952, its OnAction: the path a key press
// takes; mode 1 press, 2 release; value 1 pressed, 0 released). A new signal replaces the steps not
// yet sent.
//
// The actions are the game's action ids at the start of its action registry (services + 48, after
// the vtable: 8-byte ids whose first field is the interned name; moveleft + 8, moveright + 16,
// moveforward + 24, moveback + 32, jump + 40, sprint + 48, toggle_run + 72, toggle_crouch + 80,
// attack_primary_mouse + 272, block + 296, use + 384), found by name.
//
// Engine thread.
#pragma once

#include <cstdint>
#include <string>

namespace kcds::runtime::input::scripted {

using Logger = void (*)(const char* format, ...);

// The player input's OnAction: (the player input, the action id, mode, value).
using Deliver = void*(__fastcall*)(void* input, const char** action, unsigned mode, float value);

// A step whose action the player input does not have (a menu's key, such as
// open_inventory_screen): the client does it itself; false when it knows no such action.
using Special = bool (*)(const char* action, unsigned mode);

// The action id of a name among the registry's first actions, nullptr when it has none.
const char** action_named(const std::uint8_t* registry, const std::string& name);

// Creates the signal when the kit asks for test hooks; false otherwise.
bool install(Logger logger);

// Sends the steps that are due. registry: the game's action registry; input: the local player's
// input (nullptr outside the world: the steps wait).
void tick(const std::uint8_t* registry, void* input, double now, Deliver deliver, Special special);

}  // namespace kcds::runtime::input::scripted
