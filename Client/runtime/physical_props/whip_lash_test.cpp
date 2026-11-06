// A whip's tapered lash (physical_props/whip_lash.hpp).
#include "whip_lash.hpp"

#include <cmath>
#include <vector>

#include "kcd2s/testkit.hpp"

namespace {

using kcds::runtime::physical_props::WhipLash;
using kcds::runtime::physical_props::WhipLashSettings;

float distance(const float* a, const float* b) {
  return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) +
                   (a[2] - b[2]) * (a[2] - b[2]));
}

// The handle's up side for a lash hanging down or swung out level: the world's x or up.
const float kUpX[3] = {1, 0, 0};
const float kUpZ[3] = {0, 0, 1};

}  // namespace

KCD2S_TEST(whip_lash, hangs_its_length_from_a_still_hand) {
  WhipLash lash;
  const float hand[3] = {0, 0, 2};
  const float down[3] = {0, 0, -1};
  lash.reset(20, 1.6f, hand);
  for (int frame = 0; frame < 120; ++frame) {
    lash.step(hand, down, kUpX, 1.0f / 60.0f, -10.0f, WhipLashSettings{});
  }
  const std::vector<float>& points = lash.points();
  KCD2S_CHECK_EQ(points.size(), std::size_t(21 * 3));
  KCD2S_CHECK(distance(&points[0], hand) < 1e-4f);
  // Straight down, its whole length (each segment kept within a few millimetres).
  const float* tip = &points[20 * 3];
  KCD2S_CHECK(std::fabs(tip[2] - (2.0f - 1.6f)) < 0.03f);
  KCD2S_CHECK(std::fabs(tip[0]) < 0.01f && std::fabs(tip[1]) < 0.01f);
}

KCD2S_TEST(whip_lash, stays_above_the_ground) {
  WhipLash lash;
  const float hand[3] = {0, 0, 1};
  const float down[3] = {0, 0, -1};
  lash.reset(20, 1.6f, hand);
  for (int frame = 0; frame < 120; ++frame) {
    lash.step(hand, down, kUpX, 1.0f / 60.0f, 0.0f, WhipLashSettings{});
  }
  for (std::size_t point = 0; point < lash.points().size() / 3; ++point) {
    KCD2S_CHECK(lash.points()[point * 3 + 2] >= -1e-4f);
  }
}

KCD2S_TEST(whip_lash, a_swing_makes_the_tip_outrun_the_hand) {
  // The hand goes round a circle of 0.4 m twice a second, the handle pointing out of it: a
  // tapered lash's tip goes faster than the hand (the game's rope: slower).
  WhipLash lash;
  float hand[3] = {0.4f, 0, 2};
  lash.reset(20, 1.6f, hand);
  const float frame = 1.0f / 60.0f;
  float fastest_tip = 0.0f;
  float hand_speed = 0.0f;
  float tip_before[3] = {0, 0, 0};
  float hand_before[3] = {hand[0], hand[1], hand[2]};
  for (int index = 1; index <= 180; ++index) {
    const float angle = float(index) * frame * 2.0f * 3.14159265f * 2.0f;
    const float out[3] = {std::cos(angle), std::sin(angle), 0.0f};
    hand[0] = 0.4f * out[0];
    hand[1] = 0.4f * out[1];
    lash.step(hand, out, kUpZ, frame, -10.0f, WhipLashSettings{});
    const float* tip = &lash.points()[20 * 3];
    if (index > 60) {
      fastest_tip = std::fmax(fastest_tip, distance(tip, tip_before) / frame);
      hand_speed = distance(hand, hand_before) / frame;
    }
    for (int axis = 0; axis < 3; ++axis) {
      tip_before[axis] = tip[axis];
      hand_before[axis] = hand[axis];
    }
  }
  KCD2S_CHECK(hand_speed > 4.5f && hand_speed < 5.5f);
  KCD2S_CHECK(fastest_tip > hand_speed * 1.5f);
}

KCD2S_TEST(whip_lash, it_stays_out_of_the_bodies_it_collides_with) {
  // A lash hanging from the hand onto a body below (a capsule across under it): with the body it
  // drapes over it, without it it hangs through.
  using kcds::runtime::physical_props::LashBody;
  const float hand[3] = {0, 0, 2};
  const float down[3] = {0, 0, -1};
  LashBody body;
  body.a[0] = -0.5f;
  body.a[2] = 1.4f;
  body.b[0] = 0.5f;
  body.b[2] = 1.4f;
  body.radius = 0.15f;
  const std::vector<LashBody> bodies = {body};
  WhipLashSettings settings;
  settings.bodies = &bodies;
  WhipLash lash;
  lash.reset(20, 1.6f, hand);
  for (int frame = 0; frame < 120; ++frame) {
    lash.step(hand, down, kUpX, 1.0f / 60.0f, -10.0f, settings);
  }
  for (std::size_t point = 2; point < lash.points().size() / 3; ++point) {
    const float* at = &lash.points()[point * 3];
    const float off = std::sqrt(at[1] * at[1] + (at[2] - 1.4f) * (at[2] - 1.4f));
    const bool across = at[0] > -0.5f && at[0] < 0.5f;
    KCD2S_CHECK(!across || off >= 0.15f - 0.01f);
  }
  WhipLash through;
  through.reset(20, 1.6f, hand);
  for (int frame = 0; frame < 120; ++frame) {
    through.step(hand, down, kUpX, 1.0f / 60.0f, -10.0f, WhipLashSettings{});
  }
  KCD2S_CHECK(through.points()[20 * 3 + 2] < 1.4f);
}

KCD2S_TEST(whip_lash, a_hand_that_jumps_carries_the_lash_along) {
  // The hand placed 5 m away in one frame (a replica's clip started again): the lash goes with
  // it, every segment its length, not stretched after the hand.
  WhipLash lash;
  float hand[3] = {0, 0, 2};
  const float down[3] = {0, 0, -1};
  lash.reset(20, 1.6f, hand);
  for (int frame = 0; frame < 60; ++frame) {
    lash.step(hand, down, kUpX, 1.0f / 60.0f, -10.0f, WhipLashSettings{});
  }
  hand[0] = 5.0f;
  lash.step(hand, down, kUpX, 1.0f / 60.0f, -10.0f, WhipLashSettings{});
  const std::vector<float>& points = lash.points();
  float longest = 0.0f;
  for (std::size_t point = 0; point + 1 < points.size() / 3; ++point) {
    longest = std::fmax(longest, distance(&points[point * 3], &points[point * 3 + 3]));
  }
  KCD2S_CHECK(longest < 0.1f);
  KCD2S_CHECK(std::fabs(points[20 * 3] - 5.0f) < 0.05f);
}

KCD2S_TEST(whip_lash, posed_it_takes_its_shape_with_no_gravity) {
  // The handle level along x, up side up: shape 1 straight holds the lash out level along x
  // against gravity; a curl of 180 bends it up and back into a half circle over the handle.
  WhipLash lash;
  const float hand[3] = {0, 0, 2};
  const float out[3] = {1, 0, 0};
  lash.reset(20, 1.6f, hand);
  WhipLashSettings posed;
  posed.shape = 1.0f;
  for (int frame = 0; frame < 30; ++frame) {
    lash.step(hand, out, kUpZ, 1.0f / 60.0f, -10.0f, posed);
  }
  const float* tip = &lash.points()[20 * 3];
  KCD2S_CHECK(std::fabs(tip[0] - 1.6f) < 0.01f);
  KCD2S_CHECK(std::fabs(tip[2] - 2.0f) < 0.01f);
  posed.curl = 180.0f;
  for (int frame = 0; frame < 30; ++frame) {
    lash.step(hand, out, kUpZ, 1.0f / 60.0f, -10.0f, posed);
  }
  tip = &lash.points()[20 * 3];
  // A half circle of 1.6 m: its tip 1.6 / pi * 2 above the handle, back over it.
  KCD2S_CHECK(std::fabs(tip[0]) < 0.03f);
  KCD2S_CHECK(std::fabs(tip[2] - (2.0f + 2.0f * 1.6f / 3.14159265f)) < 0.03f);
  // Let go (shape 0), it falls with gravity again.
  posed.shape = 0.0f;
  for (int frame = 0; frame < 90; ++frame) {
    lash.step(hand, out, kUpZ, 1.0f / 60.0f, -10.0f, posed);
  }
  KCD2S_CHECK(lash.points()[20 * 3 + 2] < 1.5f);
}
