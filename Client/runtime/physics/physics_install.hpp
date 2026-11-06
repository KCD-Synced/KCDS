// The game's physical world and what stands on it at startup: the ray test the
// rob checks use, the server's level edits, its gates and its rays
// (World.raycast).
#pragma once

namespace kcds::game_client {

// Called by initialize_adapter().
void install_world_physics();

}  // namespace kcds::game_client
