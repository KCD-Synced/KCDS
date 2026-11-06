// The studio's sky: the time of day and the weather the editor sets, through the
// world clock and weather calls the server's time and weather use (Client/runtime/world: the
// clock stopped at the hour, the game's weather preset held, its rain strength).
//
//   { op: 'studioSky', hour, preset, rain }   from the editor: hour 0 to 24, the game's preset
//                                             name ('cloudless_sunny', 'overcast_rain', ...),
//                                             rain 0 to 1
//
// handle_runtime_message: any thread; tick: engine thread.
#pragma once

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::studio::sky {

bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Every frame in the studio's world: a new sky from the editor is put in once.
void tick();

}  // namespace kcds::runtime::studio::sky
