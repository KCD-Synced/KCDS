// A remote player's body does not look for targets. Each C_Player's update walks every
// actor of the world to list the ones it could aim at (WHGame 1.5.5:, from),
// so N players cost N * N checks a frame: 39% of the frame at 256 players. A
// replica's target comes from its owner, so the list stays empty for it. The local player's is
// untouched.
#pragma once

namespace kcds::game_client {

// False when a function was not found: the game then keeps its own scan, only slower.
bool install_remote_target_scan();

}  // namespace kcds::game_client
