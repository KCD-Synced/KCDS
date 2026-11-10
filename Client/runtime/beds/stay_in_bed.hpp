// Lying in a bed in a multiplayer world. The game's Sleep (E on a bed) lays the player down, and
// once they lie its sleep node asks for the skip-time screen. A multiplayer world never starts that
// screen (the server owns the time), and the node answers a screen that did not start by standing
// the player up: they got up again about two seconds after lying down.
//
// Here the sleep start is held while the local player lies in the bed in a multiplayer world: they
// stay lying (no sleep, no time skip), and the game's own "Get up" hint on the use key (E) runs the
// held sleep start, which stands them up the game's own way, as on a chair. Leaving the bed another
// way (knocked out, moved by the server, leaving the world) drops the hint.
//
// Engine thread.
#pragma once

namespace kcds::runtime::beds {

using Logger = void (*)(const char* format, ...);

// Hooks the game's sleep start; false when its function is not found.
bool install(Logger logger);

// Every frame: keeps the Get up hint while the player lies held in a bed, and drops it when they
// left it.
void frame(bool in_world);

}  // namespace kcds::runtime::beds
