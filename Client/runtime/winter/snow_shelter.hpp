// Snow stays outside: no flakes fall and no snow lies under the roof the camera stands under.
// The engine's snow stage has an occlusion map for this, but the game never fills the occluders
// it is drawn from (it rains through roofs in the stage's eyes too), so KCDS measures the roof
// itself with rays straight up: whether one is over the camera, and how far it reaches in eight
// directions. The snow shader (snow_cover.hpp) then leaves out a column of that size around the
// roof's middle. Header only, so the test runs it with a roof of its own.
#pragma once

#include <algorithm>
#include <cmath>

namespace kcds::runtime::winter::snow_shelter {

// How far the roof is looked for (metres) and in what steps.
constexpr float kFarthest = 16.0f;
constexpr float kStep = 1.0f;
// The column's edge past the farthest covered step, so the floor under the eaves stays clear.
constexpr float kMargin = 1.0f;

struct Shelter {
  bool covered = false;
  // The column's middle (x, y) and radius, when covered.
  float centre[2] = {0.0f, 0.0f};
  float radius = 0.0f;
};

// `roof_over(x, y, z)`: whether something lies straight over that point. `at`: the camera.
template <class RoofOver>
Shelter shelter_at(const float at[3], RoofOver roof_over) {
  Shelter shelter;
  if (!roof_over(at[0], at[1], at[2])) {
    return shelter;
  }
  shelter.covered = true;
  // Where the roof ends in each of eight directions.
  constexpr int kDirections = 8;
  float ends[kDirections][2];
  for (int k = 0; k < kDirections; ++k) {
    const float angle = float(k) * 3.14159265f * 2.0f / float(kDirections);
    const float dx = std::cos(angle);
    const float dy = std::sin(angle);
    float reach = 0.0f;
    for (float d = kStep; d <= kFarthest; d += kStep) {
      if (!roof_over(at[0] + dx * d, at[1] + dy * d, at[2])) {
        break;
      }
      reach = d;
    }
    ends[k][0] = at[0] + dx * reach;
    ends[k][1] = at[1] + dy * reach;
  }
  // The middle of the ends, and the farthest end from it.
  float middle[2] = {0.0f, 0.0f};
  for (const auto& end : ends) {
    middle[0] += end[0] / float(kDirections);
    middle[1] += end[1] / float(kDirections);
  }
  float radius = 0.0f;
  for (const auto& end : ends) {
    // In parentheses: Windows.h, included before this header, makes max a macro.
    radius = (std::max)(radius, std::hypot(end[0] - middle[0], end[1] - middle[1]));
  }
  shelter.centre[0] = middle[0];
  shelter.centre[1] = middle[1];
  shelter.radius = radius + kMargin;
  return shelter;
}

}  // namespace kcds::runtime::winter::snow_shelter
