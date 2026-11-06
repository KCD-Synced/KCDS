// No single-player game over in a server world: a KCDS player is knocked out, never killed, and
// the server decides when they get up. The game starts every game over in one place (WHGame
// 1.5.5:, the game-over manager's start with the reason, reached by the script
// function wh::playermodule::GameOver and by the behaviour trees' GameOver node), and
// the behaviour trees start one on their own when hostile NPCs stand around an unconscious player
// (interrupt_attack's DiedWhileUnconscious, live: a bandit's knock-out ended the game). Hooked:
// in a server world the start is refused and logged; outside one the game is its own.
#pragma once

namespace kcds::runtime::knockout::game_over {

using Logger = void (*)(const char* format, ...);

// `start` is the game-over start; `in_server_world` tells whether the client plays
// in a server's world now. False when the start is missing or the hook fails.
bool install(void* start, bool (*in_server_world)(), Logger logger);

}  // namespace kcds::runtime::knockout::game_over
