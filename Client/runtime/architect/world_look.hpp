// World Architect's time of day and weather: the author sets the hour, how fast the
// clock runs and the sky, to see the work in any light. Local mode only (local_mode.hpp): with
// no server, nothing else sets the clock or the weather.
//
//   { op: 'architectWorld', hour, speed, weather }
//     hour     0 up to (not including) 24, on the day the game's clock is on
//     speed    game seconds per real second, 0 to 1000 (0 holds the clock)
//     weather  one of the game's weather presets (cloudless_sunny, foggy_storm, ...); empty
//              leaves the game's own weather
//
// The clock and the sky are set through the same natives the server's world time and weather
// use (world/world_clock.hpp, world/world_weather.hpp); the preset is put in again before the
// game's own weather timeline takes over.
//
// handle_runtime_message: any thread. frame: engine thread, every frame of the local mode.
#pragma once

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::architect::world_look {

// True when the message was an architectWorld one (applied in the local mode only).
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Applies a time or weather the editor asked for once the game has its calendar and weather
// system, and keeps the weather.
void frame(bool in_world);

}  // namespace kcds::runtime::architect::world_look
