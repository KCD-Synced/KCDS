// The game's random events (a man by the road, a riddler, an ambush, a crier) stay off in a
// server's world: each player's game would spawn its own, which no other player sees, and a fast
// travel stopped at one ("Stop by" / "Leave him be"). wh_pl_RandomEventsAutoSpawnEnabled switches
// their spawning off ("Enables automatic spawn of random events when player enters trigger area.
// Also enables generic random events.").
#pragma once

namespace kcds::runtime::world::random_events {

// Keeps the game's random events off. Engine thread, every frame; it looks once a second.
void keep_off();

}  // namespace kcds::runtime::world::random_events
