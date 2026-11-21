// Where voice is heard from: the view camera, but not the game's screens' studio. The game's
// inventory, Player and map screens show the player in a studio of their own, the camera
// kilometres from the world (8 km on Kutnohorsko), and every talker went out of earshot while one
// was open. While the camera stands that far from the local player's own body, the listener stays
// where it last was near it.
//
// Portable: the unit test builds it.
#pragma once

#include <algorithm>

namespace kcds::runtime::voice {

// Farther than this from the player's body, the camera is not the player's view of the world.
inline constexpr float kStudioMetres = 500.0f;

// The last place heard from, as the view matrix (3 x 4, the place in its column 3).
struct ListenerPlace {
  bool known = false;
  float matrix[12] = {};
};

// The view to hear from now: the camera's, kept as the last place, unless the body's place is
// known (`body`, x y z, or null) and the camera stands more than kStudioMetres from it.
inline const float* listen_from(ListenerPlace& place, const float camera[12], const float* body) {
  if (place.known && body) {
    const float dx = camera[3] - body[0];
    const float dy = camera[7] - body[1];
    const float dz = camera[11] - body[2];
    if (dx * dx + dy * dy + dz * dz > kStudioMetres * kStudioMetres) {
      return place.matrix;
    }
  }
  std::copy(camera, camera + 12, place.matrix);
  place.known = true;
  return place.matrix;
}

}  // namespace kcds::runtime::voice
