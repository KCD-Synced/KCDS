// Where voice is heard from: the view camera, but not the game's screens' studio. The game's
// inventory, Player and map screens show the player in a studio of their own, the camera
// kilometres from the world (8 km on Kutnohorsko), and every talker went out of earshot while one
// was open. While the game shows a screen and the camera stands that far from where it last was
// in the world, the listener stays there.
//
// Portable: the unit test builds it.
#pragma once

#include <algorithm>

namespace kcds::runtime::voice {

// Farther than this from the last place, with a game screen open, the camera is the studio's.
inline constexpr float kStudioMetres = 100.0f;

// The last place heard from, as the view matrix (3 x 4, the place in its column 3).
struct ListenerPlace {
  bool known = false;
  float matrix[12] = {};
};

// The view to hear from now: the camera's, kept as the last place, unless a game screen is open
// and the camera is more than kStudioMetres from the last place.
inline const float* listen_from(ListenerPlace& place, const float camera[12], bool screen_open) {
  if (place.known && screen_open) {
    const float dx = camera[3] - place.matrix[3];
    const float dy = camera[7] - place.matrix[7];
    const float dz = camera[11] - place.matrix[11];
    if (dx * dx + dy * dy + dz * dz > kStudioMetres * kStudioMetres) {
      return place.matrix;
    }
  }
  std::copy(camera, camera + 12, place.matrix);
  place.known = true;
  return place.matrix;
}

}  // namespace kcds::runtime::voice
