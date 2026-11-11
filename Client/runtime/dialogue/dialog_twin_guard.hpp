// A guard on the game's dialogue stand-ins, WHGame 1.5.5.
//
// When a dialogue starts, the game makes a stand-in of each person in it ("DialogTwin_<name>",
// an actor of its own the dialogue plays on) and then sets each one up from its person. Making
// them stops at the first stand-in the game cannot make, but setting them up goes on over every
// person and reads each one's stand-in without asking whether it was made: a person without one
// crashed the game (an access violation reading the stand-in). The guard leaves out a person
// whose stand-in is missing, as the making already did, and says so in the log.
#pragma once

namespace kcds::runtime::dialogue::twin_guard {

using Logger = void (*)(const char* format, ...);

// The game's set-up of one person's stand-in (the dialogue, the person, the stand-in). False
// when it is missing or the hook fails.
bool install(void* set_up_twin, Logger logger);

}  // namespace kcds::runtime::dialogue::twin_guard
