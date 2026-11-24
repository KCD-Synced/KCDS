// The game's speed limiter on a ridden horse, lifted by the server (server.json horseTownLimit
// off), WHGame 1.5.5.
//
// The player's mode in a settlement switches the horse speed limiter on and off with one function
// (the mode's owner and on or off): switched on, it adds the "horseSpeedLimiter" tag to the
// player, which keeps a ridden horse from galloping. Lifted, every request to switch it on is
// turned into one to switch it off, so a limiter set before is taken away too.
//
// Engine thread.
#pragma once

namespace kcds::runtime::riding::town_gait {

using Logger = void (*)(const char* format, ...);

// Hooks the limiter switch. False when it could not be hooked.
bool install(void* limiter_switch, Logger logger);

// The server's setting: true keeps the game's limiter, false lifts it.
void set_limited(bool limited);

}  // namespace kcds::runtime::riding::town_gait
