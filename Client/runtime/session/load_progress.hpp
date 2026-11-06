// The game's own progress of a level's load (LoadingScreen), WHGame 1.5.5: its load is
// made of sections, one after the other, and each reports through
// wh::C_LoadingSection::SetProgress (section, float 0 to 1, stored at + 16). No value
// says how far the whole load is: the first section is at 1 within a second, and the load goes on
// for 15 more. The percentage counts the sections seen, out of the 14 a level's load was measured
// to have, and stays at 95 at most, so a load is never shown as finished before the world is ready.
#pragma once

namespace kcds::runtime::session::load_progress {

using Logger = void (*)(const char* format, ...);

// False when the setter is missing or the hook fails: the page then hears no percentage.
bool install(void* section_set_progress, Logger logger);

// A new world's load starts: the sections count from nothing again. Any thread.
void reset();

// The newest whole percentage if it changed since the last call, else -1. Any thread.
int take_changed();

}  // namespace kcds::runtime::session::load_progress
