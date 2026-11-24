// The game's town limit on a ridden horse's gait, switched off by the server (server.json
// horseTownLimit off), WHGame 1.5.5.
//
// The rider's input becomes the horse's command in one function (the rider's horse controller,
// the command it fills): while the rider's flag "in a town" is set (rider actor + 16, then
// + 3432, its byte at + 56), it caps the gait the command asks for at a trot, and a spur plays the
// rider's "speeding" remark. Switched off, the flag reads clear for the length of that call, so
// the horse keeps every gait; it is set back right after, for the game's other uses of it.
//
// Engine thread.
#pragma once

namespace kcds::runtime::riding::town_gait {

using Logger = void (*)(const char* format, ...);

// Hooks the rider's command builder. False when it could not be hooked.
bool install(void* command_builder, Logger logger);

// The server's setting: true keeps the game's town limit, false lifts it.
void set_limited(bool limited);

}  // namespace kcds::runtime::riding::town_gait
