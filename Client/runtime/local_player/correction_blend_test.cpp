#include "correction_blend.hpp"

#include <cmath>

#include "kcd2s/testkit.hpp"

using namespace kcds::runtime::local_player;

KCD2S_TEST(correction_blend, only_a_short_correction_is_blended) {
  KCD2S_CHECK(!blends(0.005f));
  KCD2S_CHECK(blends(0.5f));
  KCD2S_CHECK(blends(1.0f));
  KCD2S_CHECK(!blends(1.5f));
}

KCD2S_TEST(correction_blend, adds_the_whole_offset_and_no_more) {
  CorrectionBlend blend;
  const float from[3] = {10.0f, 20.0f, 30.0f};
  const float to[3] = {10.6f, 20.0f, 29.8f};
  start_blend(blend, from, to, 100.0);
  float sum[3] = {};
  float step[3] = {};
  for (int frame = 0; frame <= 60 && blend.active; ++frame) {
    step_blend(blend, 100.0 + frame / 60.0, step);
    for (int axis = 0; axis < 3; ++axis) {
      sum[axis] += step[axis];
    }
  }
  KCD2S_CHECK(!blend.active);
  KCD2S_CHECK(std::fabs(sum[0] - 0.6f) < 1e-4f);
  KCD2S_CHECK(std::fabs(sum[1]) < 1e-4f);
  KCD2S_CHECK(std::fabs(sum[2] + 0.2f) < 1e-4f);
  // Once done it adds nothing.
  step_blend(blend, 200.0, step);
  KCD2S_CHECK(step[0] == 0.0f && step[1] == 0.0f && step[2] == 0.0f);
}

KCD2S_TEST(correction_blend, stays_under_the_servers_speed_limit) {
  // The server judges steps of 0.1 s or more against 10 m/s on foot; a sprint is about 6 m/s, so a
  // blend may add at most 2.5 m/s over any such window.
  CorrectionBlend blend;
  const float from[3] = {0.0f, 0.0f, 0.0f};
  const float to[3] = {1.0f, 0.0f, 0.0f};
  start_blend(blend, from, to, 0.0);
  float added[60] = {};
  float step[3] = {};
  int frames = 0;
  while (blend.active && frames < 60) {
    step_blend(blend, (frames + 1) / 60.0, step);
    added[frames++] = step[0];
  }
  // Any 6 frames (0.1 s): the blend's own share of the distance.
  for (int start = 0; start + 6 <= frames; ++start) {
    float window = 0.0f;
    for (int frame = start; frame < start + 6; ++frame) {
      window += added[frame];
    }
    KCD2S_CHECK(window / 0.1f <= kBlendSpeed + 1e-3f);
  }
}
