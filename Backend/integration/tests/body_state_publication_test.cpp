#include "kcd2s/integration/body_state_publication.hpp"
#include "kcd2s/testkit.hpp"

using namespace kcd2s::integration;

namespace {
BodyState idle(kcd2s::local::AnimationState& animations) {
  BodyState state;
  state.revision = 1;
  state.epoch = 1;
  state.source = 1;
  state.sequence = 1;
  animations[0].sequence = 1;
  animations[0].clip = 8;
  state.animations = kcd2s::local::encode_animation(animations);
  return state;
}
}  // namespace

KCD2S_TEST(body_state_publication, idle_clocks_are_bounded_and_refresh_after_a_lost_report) {
  BodyStatePublication publication;
  kcd2s::local::AnimationState animations;
  auto state = idle(animations);
  unsigned published = 0;
  double last = -1;
  for (unsigned frame = 0; frame <= 600; ++frame) {
    const double now = frame / 60.0;
    state.sequence = frame + 1;
    animations[0].params.normalized_time = float(frame % 60) / 60;
    state.animations = kcd2s::local::encode_animation(animations);
    if (publication.publish(state, now)) {
      if (last >= 0) {
        KCD2S_CHECK(now - last <= 0.51);
      }
      last = now;
      ++published;
    }
  }
  // Even if a datagram is lost, a still-idle body supplies another complete state.
  // The first critical report also has a short fast retry window; sustained idle stays2Hz.
  KCD2S_CHECK(published >= 21 && published <= 25);
  KCD2S_CHECK(publication.publish(state, 10.5));
}

KCD2S_TEST(body_state_publication, lost_attack_start_is_repeated_before_the_idle_heartbeat) {
  BodyStatePublication publication;
  kcd2s::local::AnimationState animations;
  auto state = idle(animations);
  KCD2S_CHECK(publication.publish(state, 0));
  KCD2S_CHECK(publication.publish(state, 1));
  animations[0].sequence = 2;
  animations[0].clip = 20;
  state.animations = kcd2s::local::encode_animation(animations);
  KCD2S_CHECK(publication.publish(state, 1.001));  // This full report is lost in transit.
  KCD2S_CHECK(!publication.publish(state, 1.02));
  KCD2S_CHECK(publication.publish(state, 1.051));  // Same start retries after50ms.
  KCD2S_CHECK(publication.publish(state, 1.101));
}

KCD2S_TEST(body_state_publication, motion_is_twenty_hz_but_attack_stop_and_authority_do_not_wait) {
  BodyStatePublication publication;
  kcd2s::local::AnimationState animations;
  auto state = idle(animations);
  unsigned published = 0;
  for (unsigned frame = 0; frame <= 60; ++frame) {
    state.position.x = float(frame) * 0.1f;
    state.sequence = frame + 1;
    published += publication.publish(state, frame / 60.0);
  }
  KCD2S_CHECK_EQ(published, 21u);
  animations[0].sequence = 2;
  animations[0].clip = 12;
  state.animations = kcd2s::local::encode_animation(animations);
  KCD2S_CHECK(publication.publish(state, 1.001));
  animations[0].sequence = 3;
  animations[0].clip = -1;
  state.animations = kcd2s::local::encode_animation(animations);
  KCD2S_CHECK(publication.publish(state, 1.002));
  ++state.epoch;
  KCD2S_CHECK(publication.publish(state, 1.003));
  ++state.source;
  KCD2S_CHECK(publication.publish(state, 1.004));
  ++state.revision;
  KCD2S_CHECK(publication.publish(state, 1.005));
  state.motion_flags |= kBodyFighting;
  KCD2S_CHECK(publication.publish(state, 1.006));
}

KCD2S_TEST(body_state_publication, server_does_not_throttle_moving_packets_a_second_time) {
  BodyStatePublication publication;
  kcd2s::local::AnimationState animations;
  auto state = idle(animations);
  KCD2S_CHECK(publication.publish(state, 0, false));
  state.position.x = 1;
  KCD2S_CHECK(publication.publish(state, 0.049, false));
  state.position.x = 2;
  KCD2S_CHECK(publication.publish(state, 0.098, false));
  ++state.sequence;
  KCD2S_CHECK(!publication.publish(state, 0.12, false));
  KCD2S_CHECK(publication.publish(state, 0.6, false));
}

KCD2S_TEST(body_state_publication, riding_and_release_keep_progress_fresh_beyond_the_start_retry) {
  BodyStatePublication sender, server;
  kcd2s::local::AnimationState animations;
  auto state = idle(animations);
  for (unsigned sample = 0; sample <= 20; ++sample) {
    const double now = double(sample) / 20;
    state.sequence = sample + 1;
    animations[0].params.normalized_time = float(sample % 20) / 20;
    state.animations = kcd2s::local::encode_animation(animations);
    KCD2S_CHECK(sender.publish(state, now, true, true));
    KCD2S_CHECK(server.publish(state, now + 0.01, false, true));
  }
  // Dismount advances the epoch. It must not erase the four-second progress tail.
  ++state.epoch;
  for (unsigned sample = 21; sample < 100; ++sample) {
    const double now = double(sample) / 20;
    ++state.sequence;
    animations[0].params.normalized_time = float(sample % 20) / 20;
    state.animations = kcd2s::local::encode_animation(animations);
    KCD2S_CHECK(sender.publish(state, now, true, false));
    // Alternating delivery jitter must not halve the normal sender's update rate.
    const double arrival = now + (sample % 2 ? 0.011 : 0.009);
    KCD2S_CHECK(server.publish(state, arrival, false, false));
  }
  KCD2S_CHECK(!sender.publish(state, 5.05));
  KCD2S_CHECK(!server.publish(state, 5.061, false));
  KCD2S_CHECK(sender.publish(state, 5.5));
  KCD2S_CHECK(server.publish(state, 5.51, false));
}

KCD2S_TEST(body_state_publication, fighting_native_state_keeps_existing_clip_progress_fresh) {
  BodyStatePublication publication;
  kcd2s::local::AnimationState animations;
  auto state = idle(animations);
  state.motion_flags = kBodyFighting;
  for (unsigned sample = 0; sample < 40; ++sample) {
    const double now = double(sample) / 20;
    ++state.sequence;
    animations[0].params.normalized_time = float(sample % 20) / 20;
    state.animations = kcd2s::local::encode_animation(animations);
    KCD2S_CHECK(publication.publish(state, now));
  }
}
