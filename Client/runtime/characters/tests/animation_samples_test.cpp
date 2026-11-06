#include "../animation_samples.hpp"

#include <limits>

#include "kcd2s/testkit.hpp"

using namespace kcds::runtime::characters;

namespace {
AnimationBodySample body(std::uint32_t id = 1) {
  AnimationBodySample value;
  value.engine_id = id;
  value.entity = 100 + id;
  value.character = 200 + id;
  value.skeleton = 300 + id;
  value.layers[0] = {400 + id, 12, 0.25f};
  return value;
}
}  // namespace

KCD2S_TEST(animation_samples, repeated_samples_distinguish_still_body_face_and_loop_progress) {
  AnimationSamples samples;
  int system;
  auto value = body();
  value.layers[11] = {800, 30, 0.2f};
  samples.begin(&system);
  KCD2S_CHECK(samples.observe(value)->comparable_layers == 0);
  samples.finish();
  samples.begin(&system);
  auto comparison = samples.observe(value);
  KCD2S_CHECK(comparison->comparable_layers == 2);
  KCD2S_CHECK(comparison->progressing_layers == 0);
  KCD2S_CHECK(comparison->body_comparable);
  samples.finish();
  samples.begin(&system);
  value.layers[11].progress = 0.4f;
  comparison = samples.observe(value);
  KCD2S_CHECK(comparison->progressing_layers == 1);
  KCD2S_CHECK(!comparison->body_progressing);
  samples.finish();
  samples.begin(&system);
  value.layers[0].progress = 0.05f;  // A looping clip may wrap between samples.
  comparison = samples.observe(value);
  KCD2S_CHECK(comparison->body_progressing);
  samples.finish();
}

KCD2S_TEST(animation_samples, replaced_native_identities_and_clips_do_not_prove_progress) {
  for (unsigned replacement = 0; replacement < 5; ++replacement) {
    AnimationSamples samples;
    int system;
    auto value = body();
    samples.begin(&system);
    samples.observe(value);
    samples.finish();
    if (replacement == 0) {
      ++value.entity;
    } else if (replacement == 1) {
      ++value.character;
    } else if (replacement == 2) {
      ++value.skeleton;
    } else if (replacement == 3) {
      ++value.layers[0].entry;
    } else {
      ++value.layers[0].clip;
    }
    value.layers[0].progress = 0.75f;
    samples.begin(&system);
    const auto comparison = samples.observe(value);
    KCD2S_CHECK(comparison->comparable_layers == 0);
    KCD2S_CHECK(comparison->progressing_layers == 0);
    samples.finish();
  }
}

KCD2S_TEST(animation_samples, absent_bodies_world_replacement_and_invalid_progress_reset_evidence) {
  AnimationSamples samples;
  int system, other;
  auto value = body();
  samples.begin(&system);
  samples.observe(value);
  samples.finish();
  samples.begin(&system);
  samples.finish();  // Absence drops the old identity rather than retaining a stale sample.
  KCD2S_CHECK(samples.retained() == 0);
  value.layers[0].progress = 0.5f;
  samples.begin(&system);
  KCD2S_CHECK(samples.observe(value)->comparable_layers == 0);
  samples.finish();
  value.layers[0].progress = 0.75f;
  samples.begin(&other);
  KCD2S_CHECK(samples.observe(value)->comparable_layers == 0);
  samples.finish();
  for (const float invalid : {-1.0f, 1.1f, std::numeric_limits<float>::quiet_NaN()}) {
    samples.begin(&other);
    value.layers[0].progress = invalid;
    KCD2S_CHECK(samples.observe(value)->comparable_layers == 0);
    samples.finish();
  }
}

KCD2S_TEST(animation_samples, sustained_churn_has_a_fixed_bound_and_no_duplicate_evidence) {
  AnimationSamples samples;
  int system;
  for (std::uint32_t round = 0; round < 3; ++round) {
    samples.begin(&system);
    for (std::uint32_t id = 1; id <= kMaximumAnimationSamples; ++id) {
      const auto value = body(id + round * kMaximumAnimationSamples);
      KCD2S_CHECK(samples.observe(value).has_value());
      KCD2S_CHECK(!samples.observe(value).has_value());
    }
    KCD2S_CHECK(!samples.observe(body(50000)).has_value());
    samples.finish();
    KCD2S_CHECK(samples.retained() == kMaximumAnimationSamples);
  }
}
