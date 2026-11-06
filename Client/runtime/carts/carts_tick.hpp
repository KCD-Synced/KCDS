// The carts the server placed and their view of the local player.
#pragma once

#include "kcd2s/integration/carts.hpp"

namespace kcds::game_client {

// Where the local player's body stands, and putting it there.
bool cart_local_pose(float position[3], float turn[4]);
void cart_place_local(const float position[3], const float turn[4]);
// The terrain's height under a world point, from `above` down; false when none was found.
bool cart_ground_height(float x, float y, float above, float& z);
// Whether something solid other than the terrain lies between two world points.
bool cart_blocked(const float from[3], const float to[3], std::uint32_t own);
// Sends a cart report to the server.
bool cart_report(const kcd2s::integration::CartReport& report);
// The server's cart orders, in the order it sent them, and the carts' frame.
void tick_carts();

}  // namespace kcds::game_client
