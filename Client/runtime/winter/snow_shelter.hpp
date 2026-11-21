// Snow stays outside: no flakes fall and no snow lies under the roof the camera stands under. With
// the camera in the open nothing is bared (measured from the camera's height, the nearest roof
// came out wrong from above its eaves and bared the house and its yard only near it).
// The engine's snow stage has an occlusion map for this, but the game never fills the occluders
// it is drawn from (it rains through roofs in the stage's eyes too), so KCDS measures the roof
// itself with rays straight up: whether one is over the camera, and how far it reaches in sixteen
// directions. A box turned along the roof is fitted to those ends, and the snow shader
// (snow_cover.hpp) leaves that box without snow. A box, not a circle: a roof joined to the next
// house reaches far one way only, and a circle of that reach took the snow off the street beside
// it. Header only, so the test runs it with a roof of its own.
#pragma once

#include <algorithm>
#include <cmath>

namespace kcds::runtime::winter::snow_shelter {

// How far the roof is looked for (metres) and in what steps.
constexpr float kFarthest = 16.0f;
constexpr float kStep = 1.0f;
// The box's edge past the farthest covered step: the roof's real edge lies up to a step further.
constexpr float kMargin = 1.0f;
constexpr int kDirections = 16;

struct Shelter {
  bool covered = false;
  // The roof is over the camera itself (no flakes fall then), not one beside it.
  bool over_camera = false;
  // When covered: the box's middle (x, y), its half length and half width (metres), and the
  // direction of its length (radians from the x axis, 0 to pi).
  float centre[2] = {0.0f, 0.0f};
  float half_length = 0.0f;
  float half_width = 0.0f;
  float turn = 0.0f;
};

// `roof_over(x, y, z)`: whether something lies straight over that point. `at`: the camera.
template <class RoofOver>
Shelter shelter_at(const float at[3], RoofOver roof_over) {
  Shelter shelter;
  if (!roof_over(at[0], at[1], at[2])) {
    return shelter;
  }
  shelter.covered = true;
  // Where the roof ends in each direction.
  constexpr float kPi = 3.14159265f;
  float ends[kDirections][2];
  for (int k = 0; k < kDirections; ++k) {
    const float angle = float(k) * kPi * 2.0f / float(kDirections);
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
  // The roof's length lies along the ends' greatest spread (their principal axis).
  float middle[2] = {0.0f, 0.0f};
  for (const auto& end : ends) {
    middle[0] += end[0] / float(kDirections);
    middle[1] += end[1] / float(kDirections);
  }
  float xx = 0.0f;
  float yy = 0.0f;
  float xy = 0.0f;
  for (const auto& end : ends) {
    const float x = end[0] - middle[0];
    const float y = end[1] - middle[1];
    xx += x * x;
    yy += y * y;
    xy += x * y;
  }
  float turn = 0.5f * std::atan2(2.0f * xy, xx - yy);
  if (turn < 0.0f) {
    turn += kPi;
  }
  const float along[2] = {std::cos(turn), std::sin(turn)};
  const float across[2] = {-along[1], along[0]};
  // The ends' extent along and across the length gives the box.
  float low[2] = {0.0f, 0.0f};
  float high[2] = {0.0f, 0.0f};
  for (const auto& end : ends) {
    const float x = end[0] - middle[0];
    const float y = end[1] - middle[1];
    const float on_length = x * along[0] + y * along[1];
    const float on_width = x * across[0] + y * across[1];
    // In parentheses: Windows.h, included before this header, makes min and max macros.
    low[0] = (std::min)(low[0], on_length);
    high[0] = (std::max)(high[0], on_length);
    low[1] = (std::min)(low[1], on_width);
    high[1] = (std::max)(high[1], on_width);
  }
  const float mid_length = (low[0] + high[0]) * 0.5f;
  const float mid_width = (low[1] + high[1]) * 0.5f;
  shelter.centre[0] = middle[0] + along[0] * mid_length + across[0] * mid_width;
  shelter.centre[1] = middle[1] + along[1] * mid_length + across[1] * mid_width;
  shelter.half_length = (high[0] - low[0]) * 0.5f + kMargin;
  shelter.half_width = (high[1] - low[1]) * 0.5f + kMargin;
  shelter.turn = turn;
  return shelter;
}

// How far a camera in the open may be from the box it keeps (metres): it keeps the box of the
// building it walks around until it walks away.
constexpr float kKeepNear = 8.0f;

// How far the point (x, y) lies outside the box, 0 inside it.
inline float distance_outside(const Shelter& box, float x, float y) {
  const float along[2] = {std::cos(box.turn), std::sin(box.turn)};
  const float dx = x - box.centre[0];
  const float dy = y - box.centre[1];
  const float on_length = std::fabs(dx * along[0] + dy * along[1]) - box.half_length;
  const float on_width = std::fabs(-dx * along[1] + dy * along[0]) - box.half_width;
  const float out_length = (std::max)(on_length, 0.0f);
  const float out_width = (std::max)(on_width, 0.0f);
  return std::sqrt(out_length * out_length + out_width * out_width);
}

// Whether the box shown stays rather than the one just measured. The ground bare under a roof is
// measured from where the camera is, so a new measure moved its edge a little with every step, and
// in the open the nearest roof went from one building to the next as the camera passed them: the
// snow on the ground came and went around the player. A box stays while the camera is inside it
// (a roof over the camera measured again is the same roof), or while the camera in the open is
// still near it and no roof is over the camera; another building's box comes once the camera has
// walked away from this one.
inline bool keep_shown(const Shelter& shown, const Shelter& measured, float x, float y) {
  if (!shown.covered) {
    return false;
  }
  const float outside = distance_outside(shown, x, y);
  if (measured.over_camera) {
    return shown.over_camera && outside == 0.0f;
  }
  return outside <= kKeepNear;
}

}  // namespace kcds::runtime::winter::snow_shelter
