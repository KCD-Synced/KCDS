// The walking preference a server puts on the local player
// (Player.setPace's walkByDefault): walking is the pace the player keeps coming back to.
//
// The game's walk/jog toggle is the player input's byte + 124 (C_PlayerInput, actor + 2952, whose
// actor is at + 16; 1 jog, 0 walk). Its toggle_run handler flips the byte when the key
// is released, and a sprint sets it to jog the moment the sprint starts and leaves it there. With
// the preference on, the client puts walk in that byte when the rule arrives, for every new body,
// and once a sprint is over when the player walked before it (a player who chose to jog keeps
// jogging). The toggle key works as usual in between.
//
// Engine thread (on_action comes from the player input's OnAction, on the engine thread too).
#pragma once

namespace kcds::runtime::movement::walk_default {

using Logger = void (*)(const char* format, ...);

void install(Logger logger);

// Every frame: the local actor (nullptr outside a server world) and whether the preference holds.
void tick(void* actor, bool walk_by_default);

// The local player's input actions, before the game handles them.
void on_action(const char* action, unsigned mode);

}  // namespace kcds::runtime::movement::walk_default
