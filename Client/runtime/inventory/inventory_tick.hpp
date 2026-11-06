// The local player's inventory against the server each frame: registered item types and names,
// the census, the server's changes applied in the game and the game's own changes reported.
#pragma once

namespace kcds::game_client {

void tick_inventory(void* local_actor);

// The local body is going onto another skeleton: the game takes off what it wore with
// the old character, so nothing of the game's own changes is reported until redress_local_body().
void hold_local_inventory();

// The body is on its new skeleton: what the last stock wears goes back on, the weapons into the
// quickslots that held them, and the reports go on once they are.
void redress_local_body();

}  // namespace kcds::game_client
