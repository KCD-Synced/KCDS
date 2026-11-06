// Two players' bodies touching does nobody harm. The engine's actor collision
// turns every touch of two actors into blows on both: in a crowd the
// bumps took a standing player's health and knocked them out. Bodies still block each other, which
// is physics and does not go through this function. A horse's trample is untouched: the horse is
// not a player's body.
#pragma once

namespace kcds::game_client {

// False when the function was not found: bodies then keep hurting each other.
bool install_body_collisions();

}  // namespace kcds::game_client
