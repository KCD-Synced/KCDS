// The world clock the server sets (kcd2s/integration/world_time.hpp).
//
// The game keeps its time in its own calendar (C_Calendar): a 64-bit count of game milliseconds
// at + 72 (day * 86400000 + hour * 3600000) and a speed, its time ratio (game seconds per real
// second). The calendar hangs off the game's global object (+ 0x88). The server's time is set
// through the two calls the game's own script functions use (Calendar.SetWorldTime and
// Calendar.SetWorldTimeRatio), no script runs:
//
//   - set_time (calendar, game milliseconds) moves the clock, but only forward (it
//     ignores an earlier time) and it raises the day-change event when the day changes. To move
//     it back the stored time is first put 1 ms before the target, so the same call accepts it;
//   - set_ratio (calendar, ratio as a float) sets the speed; 0 freezes the clock.
//
// Engine thread.
#pragma once

#include "kcd2s/integration/world_time.hpp"

namespace kcds::runtime::world {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* const* game_globals = nullptr;  // the slot of the game's global object
  void* set_time = nullptr;
  void* set_ratio = nullptr;
};

// False when a native is missing; the clock then stays the game's own.
bool install(const Natives& natives, Logger logger);

// Sets the game's clock to the server's time and speed. A clock that is already within a
// second of it is left alone (the server's correction about every minute would otherwise
// make the calendar redo its updates each time); false while the game has no calendar yet.
bool apply(const kcd2s::integration::WorldTime& time);

// The day the game's clock is on, or -1 while the game has no calendar (World Architect's time of
// day keeps the day).
long long current_day();

}  // namespace kcds::runtime::world
