#pragma once
// Starting an Epic Games Store copy with the KCDS client. The game must be started by the Epic
// launcher: it hands the game its sign-in, which the game's own Epic session (and the KCDS
// licence proof read from it) needs. This tool never sees or passes those arguments.
//
// The Epic launcher starts the game as its own child, not suspended, so the client is loaded as
// soon as the game's process appears. The launch arguments of a direct start (+sv_port and the
// audio memory pool) can't be given this way; the game keeps its own settings.
#include <string>

#include "store_copy.hpp"

namespace kcds::runtime::launch {

// How long the Epic launcher may take to start the game (it may start, update and sign in first).
inline constexpr unsigned long kStoreLaunchTimeoutMs = 90000;

// Starts the copy whose KingdomCome.exe is game through the Epic launcher and loads dll into it.
// Returns the launch-client exit code: 0 started, 1 the client could not be loaded, 4 the game
// closed right away, 5 the Epic launcher did not start this game in time.
int launch_epic(const EpicIds& ids, const std::wstring& game, const std::wstring& dll);

}  // namespace kcds::runtime::launch
