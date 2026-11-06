// Remote players shown smoothly: the replica timeline.
#include "kcd2s/integration/replica_timeline.hpp"
#include "kcd2s/integration/player_replica_timeline.hpp"
#include "kcd2s/replication/snapshot.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "kcd2s/testkit.hpp"

using kcd2s::core::Quat;
using kcd2s::core::Vec3;
using kcd2s::integration::ReplicaTimeline;
using kcd2s::integration::TimelinePose;

namespace {

constexpr float kTeleportMeters = 8.0f;
constexpr float kSpeed = 6.0f;  // metres per second along x

// The owner at owner time `seconds`: walking along x, a 1 m jump arc every 2 s.
TimelinePose owner_pose(double seconds) {
  TimelinePose pose;
  pose.position.x = float(seconds) * kSpeed;
  const double phase = std::fmod(seconds, 2.0);
  pose.position.z = phase < 0.8 ? float(5.0 * phase * (0.8 - phase) / 0.8) : 0.0f;
  return pose;
}

// The owner's n-th 60 Hz sample on its millisecond clock, starting at `start_ms`.
std::int64_t owner_ms(std::int64_t start_ms, int n) {
  return start_ms + std::int64_t(std::llround(n * 1000.0 / 60.0));
}

}  // namespace

KCD2S_TEST(replica_timeline, nothing_before_the_first_sample) {
  ReplicaTimeline<int> timeline(kTeleportMeters);
  TimelinePose pose;
  KCD2S_CHECK(timeline.shown(1.0, pose) == nullptr);
}

KCD2S_TEST(replica_timeline, jittered_deliveries_show_the_owner_a_delay_behind) {
  // 60 Hz owner samples delivered 40-120 ms late; the replica is shown every 7 ms (144 fps).
  ReplicaTimeline<int> timeline(kTeleportMeters);
  std::mt19937 rng(3);
  std::uniform_real_distribution<double> latency(0.04, 0.12);
  struct Delivery {
    double at;
    std::int64_t owner_ms;
  };
  std::vector<Delivery> deliveries;
  for (int i = 0; i < 600; ++i) {
    const std::int64_t ms = owner_ms(10000, i);
    deliveries.push_back({ms / 1000.0 + latency(rng), ms});
  }
  std::sort(deliveries.begin(), deliveries.end(),
            [](const Delivery& a, const Delivery& b) { return a.at < b.at; });
  std::size_t next = 0;
  double worst = 0.0;
  float last_x = -1.0f;
  bool forward = true;
  for (double now = 11.5; now < 19.0; now += 0.007) {
    while (next < deliveries.size() && deliveries[next].at <= now) {
      const auto& d = deliveries[next++];
      timeline.add(d.owner_ms, 0, owner_pose(d.owner_ms / 1000.0), d.at);
    }
    TimelinePose pose;
    KCD2S_CHECK(timeline.shown(now, pose) != nullptr);
    // The fastest delivery took about 40 ms: the replica shows the owner 40 ms and its delay ago.
    const double expected_owner = now - 0.04 - timeline.delay();
    const TimelinePose expected = owner_pose(expected_owner);
    const double error = std::max(std::fabs(pose.position.x - expected.position.x),
                                  std::fabs(pose.position.z - expected.position.z));
    worst = std::max(worst, error);
    if (pose.position.x < last_x) {
      forward = false;  // never steps back
    }
    last_x = pose.position.x;
  }
  std::printf("jitter: worst %.4f m\n", worst);
  KCD2S_CHECK(forward);
  // Within 3.5 cm of the owner's path that long ago (the fastest delivery is not exactly 40 ms),
  // and the delay has grown to cover the 80 ms the deliveries vary.
  KCD2S_CHECK(worst < 0.035);
  KCD2S_CHECK(timeline.delay() > 0.07);
}

KCD2S_TEST(replica_timeline, a_stalled_server_still_gives_steady_movement) {
  // Deliveries 50 ms after the owner, but between 21.0 and 21.09 nothing comes, then everything at
  // once: the replica keeps moving at about the owner's speed instead of stopping and jumping.
  ReplicaTimeline<int> timeline(kTeleportMeters);
  double max_speed = 0.0;
  double min_speed = 1e9;
  double previous_x = -1.0;
  double previous_now = 0.0;
  int sent = 0;
  for (double now = 20.0; now < 23.0; now += 0.007) {
    const double delivered_until = (now >= 21.0 && now < 21.09) ? 21.0 - 0.05 : now - 0.05;
    while (owner_ms(19900, sent) / 1000.0 <= delivered_until) {
      const std::int64_t ms = owner_ms(19900, sent++);
      timeline.add(ms, 0, owner_pose(ms / 1000.0), now);
    }
    TimelinePose pose;
    timeline.shown(now, pose);
    if (previous_x >= 0.0 && now > 20.5) {
      const double speed = (pose.position.x - previous_x) / (now - previous_now);
      max_speed = std::max(max_speed, speed);
      min_speed = std::min(min_speed, speed);
    }
    previous_x = pose.position.x;
    previous_now = now;
  }
  std::printf("stall: speed %.3f to %.3f m/s\n", min_speed, max_speed);
  // About the owner's speed throughout: no stop, no jump.
  KCD2S_CHECK(max_speed < kSpeed * 1.15);
  KCD2S_CHECK(min_speed > kSpeed * 0.85);
}

KCD2S_TEST(replica_timeline, a_teleport_is_shown_at_once) {
  ReplicaTimeline<int> timeline(kTeleportMeters);
  for (int i = 0; i < 30; ++i) {
    const std::int64_t ms = owner_ms(5000, i);
    timeline.add(ms, 1, owner_pose(ms / 1000.0), ms / 1000.0 + 0.05);
  }
  TimelinePose far;
  far.position = Vec3{500.0f, 0.0f, 0.0f};
  const std::int64_t ms = owner_ms(5000, 30);
  timeline.add(ms, 2, far, ms / 1000.0 + 0.05);
  TimelinePose pose;
  const int* state = timeline.shown(ms / 1000.0 + 0.05, pose);
  KCD2S_CHECK(state && *state == 2);
  KCD2S_CHECK_EQ(pose.position.x, 500.0f);
  KCD2S_CHECK_EQ(timeline.sample_count(), std::size_t{1});
}

KCD2S_TEST(replica_timeline, running_dry_extrapolates_briefly_then_stands) {
  ReplicaTimeline<int> timeline(kTeleportMeters);
  std::int64_t last_ms = 0;
  for (int i = 0; i < 20; ++i) {
    last_ms = owner_ms(30000, i);
    TimelinePose walking;
    walking.position.x = float(last_ms / 1000.0) * kSpeed;
    timeline.add(last_ms, 0, walking, last_ms / 1000.0 + 0.05);
  }
  const double last = last_ms / 1000.0;
  const float last_x = float(last) * kSpeed;
  TimelinePose pose;
  // 50 ms past the newest sample: still walking.
  timeline.shown(last + 0.05 + ReplicaTimeline<int>::kDelaySeconds + 0.05, pose);
  KCD2S_CHECK(std::fabs(pose.position.x - (last_x + kSpeed * 0.05f)) < 0.01f);
  // A second later: stopped 100 ms past the newest sample.
  timeline.shown(last + 1.0, pose);
  KCD2S_CHECK(std::fabs(pose.position.x - (last_x + kSpeed * 0.1f)) < 0.01f);
}

KCD2S_TEST(replica_timeline, state_and_rotation_come_from_the_moment_shown) {
  ReplicaTimeline<int> timeline(kTeleportMeters);
  TimelinePose a;
  a.rotation = Quat{0.0f, 0.0f, 0.0f, 1.0f};
  TimelinePose b;
  b.rotation = Quat{0.0f, 0.0f, std::sqrt(0.5f), std::sqrt(0.5f)};  // 90 degrees about z
  timeline.add(1000, 7, a, 1.05);
  timeline.add(1100, 8, b, 1.15);
  TimelinePose pose;
  // Halfway between the samples on the owner's clock (offset 50 ms and the delay).
  const int* state = timeline.shown(1.05 + ReplicaTimeline<int>::kDelaySeconds + 0.05, pose);
  KCD2S_CHECK(state && *state == 7);
  const float yaw = 2.0f * std::atan2(pose.rotation.z, pose.rotation.w);
  KCD2S_CHECK(std::fabs(yaw - 0.7853982f) < 0.01f);  // 45 degrees
}

KCD2S_TEST(replica_timeline, an_owner_clock_that_restarts_starts_over) {
  ReplicaTimeline<int> timeline(kTeleportMeters);
  timeline.add(900000, 1, owner_pose(0.0), 1.0);
  timeline.add(900017, 1, owner_pose(0.017), 1.02);
  // The owner reconnected: its clock is far behind the previous samples.
  KCD2S_CHECK(timeline.add(5000, 2, owner_pose(0.0), 1.04));
  TimelinePose pose;
  const int* state = timeline.shown(1.04, pose);
  KCD2S_CHECK(state && *state == 2);
  // An old, repeated sample is ignored.
  KCD2S_CHECK(!timeline.add(5000, 3, owner_pose(0.0), 1.05));
}

KCD2S_TEST(replica_timeline, one_coalesced_idle_sample_settles_exactly_without_future_heartbeats) {
  ReplicaTimeline<int> timeline(kTeleportMeters);
  timeline.add(1000, 1, {{0, 0, 0}, {}}, 1.05);
  timeline.add(1050, 1, {{.3f, 0, 0}, {}}, 1.10);
  // The first terminal packet at1100 was lost. A complete idle refresh arrives at1200;
  // no second idle sample may be assumed to arrive merely to cancel inferred velocity.
  timeline.add(1200, 2, {{.4f, 0, 0}, {}}, 1.25, true);
  TimelinePose pose;
  const auto* state = timeline.shown(1.36, pose);
  KCD2S_CHECK(state && *state == 2);
  KCD2S_CHECK_EQ(pose.position.x, .4f);
  timeline.shown(31.36, pose);
  KCD2S_CHECK_EQ(pose.position.x, .4f);
}

KCD2S_TEST(replica_timeline,
           resume_after_long_idle_does_not_interpolate_or_measure_speed_over_gap) {
  ReplicaTimeline<int> timeline(kTeleportMeters);
  timeline.add(1000, 1, {{1, 0, 0}, {}}, 1.05, true);
  timeline.add(31000, 2, {{1.1f, 0, 0}, {}}, 31.05);
  timeline.add(31017, 2, {{1.202f, 0, 0}, {}}, 31.067);
  TimelinePose pose;
  const auto* state = timeline.shown(31.05, pose);
  KCD2S_CHECK(state && *state == 1);
  KCD2S_CHECK_EQ(pose.position.x, 1.0f);
  state = timeline.shown(31.101, pose);
  KCD2S_CHECK(state && *state == 2);
  KCD2S_CHECK(std::fabs(pose.position.x - 1.106f) < .005f);
  timeline.shown(31.15, pose);
  KCD2S_CHECK(std::fabs(pose.position.x - 1.4f) < .005f);
}

namespace {
using PlayerState = kcd2s::replication::Mirror::Entity;
using PlayerTimeline = kcd2s::integration::PlayerReplicaTimeline<PlayerState>;
namespace local = kcd2s::local;

PlayerState player_state(std::int64_t clock, float x = 0) {
  const auto schema = local::player_schema();
  kcd2s::replication::World world(schema);
  const auto id = local::spawn_player(world);
  PlayerState state;
  state.type = kcd2s::core::type_id("NetworkPlayer");
  for (const auto& field : schema.get(state.type)->fields) {
    state.fields.push_back(*world.read(id, field.id));
  }
  state.fields[local::MotionClock] = clock;
  state.fields[local::Position] = Vec3{x, 0, 0};
  state.fields[local::PoseValid] = true;
  state.fields[local::MotionValid] = true;
  state.fields[local::Posture] = kcd2s::integration::posture_field({});
  return state;
}
float player_x(const PlayerState& state) {
  return std::get<Vec3>(state.fields[local::Position]).x;
}
}  // namespace

KCD2S_TEST(replica_timeline,
           idle_player_keeps_latest_server_fields_and_returns_without_state_copy) {
  PlayerTimeline timeline(kTeleportMeters);
  PlayerState output;
  auto latest = player_state(1000, 2);
  KCD2S_CHECK(kcd2s::integration::PlayerInputPublication::idle(latest.fields));
  timeline.shown(latest, 1.05, output);
  latest.fields[local::Health] = 25.0f;
  latest.fields[local::Outfit] = std::string{"same-clock appearance correction"};
  const auto& corrected = timeline.shown(latest, 1.30, output);
  KCD2S_CHECK(&corrected == &latest);
  KCD2S_CHECK_EQ(std::get<float>(corrected.fields[local::Health]), 25.0f);
  const auto& idle = timeline.shown(latest, 31.05, output);
  KCD2S_CHECK(&idle == &latest);
  KCD2S_CHECK_EQ(player_x(idle), 2.0f);
  // The corrected payload also survives when that idle sample becomes a delayed predecessor.
  latest.fields[local::MotionClock] = std::int64_t{31000};
  latest.fields[local::Velocity] = Vec3{1, 0, 0};
  latest.fields[local::Position] = Vec3{2.1f, 0, 0};
  latest.fields[local::Outfit] = std::string{"new action appearance"};
  const auto& resumed = timeline.shown(latest, 31.05, output);
  KCD2S_CHECK_EQ(player_x(resumed), 2.0f);
  KCD2S_CHECK_EQ(std::get<std::string>(resumed.fields[local::Outfit]),
                 std::string("same-clock appearance correction"));
}

KCD2S_TEST(replica_timeline,
           server_health_is_current_while_owner_motion_is_delayed_and_epoch_resets) {
  PlayerTimeline timeline(kTeleportMeters);
  PlayerState output;
  auto latest = player_state(1000);
  timeline.shown(latest, 1.05, output);
  latest.fields[local::MotionClock] = std::int64_t{1100};
  latest.fields[local::Position] = Vec3{.6f, 0, 0};
  latest.fields[local::Velocity] = Vec3{6, 0, 0};
  latest.fields[local::Health] = 40.0f;
  const auto& delayed = timeline.shown(latest, 1.15, output);
  KCD2S_CHECK(player_x(delayed) < .01f);
  KCD2S_CHECK_EQ(std::get<float>(delayed.fields[local::Health]), 40.0f);
  latest.epoch = 1;  // Same owner clock, but a new authoritative scope must not keep old poses.
  latest.fields[local::Position] = Vec3{3, 0, 0};
  const auto& forced = timeline.shown(latest, 1.16, output);
  KCD2S_CHECK_EQ(forced.epoch, std::uint8_t{1});
  KCD2S_CHECK_EQ(player_x(forced), 3.0f);
}

KCD2S_TEST(replica_timeline, long_idle_turn_and_attack_start_keep_owner_pose_and_action_atomic) {
  PlayerTimeline timeline(kTeleportMeters);
  PlayerState output;
  auto latest = player_state(1000);
  latest.fields[local::Animations] = std::string{"idle"};
  timeline.shown(latest, 1.05, output);
  latest.fields[local::MotionClock] = std::int64_t{31000};
  latest.fields[local::Animations] = std::string{"attack"};
  latest.fields[local::Rotation] = Quat{0, 0, std::sqrt(.5f), std::sqrt(.5f)};
  latest.fields[local::Posture] = kcd2s::integration::posture_field({false, false, true});
  timeline.shown(latest, 31.05, output);
  const auto& waiting = timeline.shown(latest, 31.09, output);
  KCD2S_CHECK(std::get<Quat>(waiting.fields[local::Rotation]) == Quat{});
  KCD2S_CHECK_EQ(std::get<std::string>(waiting.fields[local::Animations]), std::string("idle"));
  const auto& attack = timeline.shown(latest, 31.11, output);
  KCD2S_CHECK(std::get<Quat>(attack.fields[local::Rotation]) ==
              std::get<Quat>(latest.fields[local::Rotation]));
  KCD2S_CHECK_EQ(std::get<std::string>(attack.fields[local::Animations]), std::string("attack"));
}

KCD2S_TEST(replica_timeline,
           player_clock_reset_invalid_clock_and_untimed_correction_drop_old_pose) {
  PlayerTimeline timeline(kTeleportMeters);
  PlayerState output;
  auto latest = player_state(40000, 1);
  timeline.shown(latest, 40.05, output);
  latest.fields[local::MotionClock] = std::int64_t{1000};
  latest.fields[local::Position] = Vec3{2, 0, 0};
  KCD2S_CHECK_EQ(player_x(timeline.shown(latest, 40.10, output)), 2.0f);
  latest.fields[local::Position] = Vec3{3, 0, 0};  // Untimed correction, below teleport threshold.
  KCD2S_CHECK_EQ(player_x(timeline.shown(latest, 40.11, output)), 3.0f);
  latest.fields[local::MotionClock] = std::int64_t{0};
  timeline.shown(latest, 40.12, output);
  latest.fields[local::MotionClock] = std::int64_t{900};
  latest.fields[local::Position] = Vec3{4, 0, 0};
  KCD2S_CHECK_EQ(player_x(timeline.shown(latest, 40.13, output)), 4.0f);
  timeline.clear();  // Disconnect/world reset, even if a subsequent clock/epoch are reused.
  latest.fields[local::Position] = Vec3{5, 0, 0};
  KCD2S_CHECK_EQ(player_x(timeline.shown(latest, 50.0, output)), 5.0f);
}

KCD2S_TEST(replica_timeline, rare_samples_move_a_body_without_stops_or_teleports) {
  // 6 m/s with a sample every 1.0 s: each step is over the teleport distance.
  ReplicaTimeline<int> timeline(5.0f);
  const Quat still{0.0f, 0.0f, 0.0f, 1.0f};
  double next_sample = 0.0;
  int sent = 0;
  double previous = -1.0;
  double slowest = 1000.0;
  for (double now = 0.0; now < 30.0; now += 1.0 / 30.0) {
    while (next_sample <= now) {
      const float x = float(next_sample * 6.0);
      timeline.add(std::int64_t(next_sample * 1000.0), sent++, {{x, 0.0f, 0.0f}, still}, now);
      next_sample += 1.0;
    }
    TimelinePose pose;
    KCD2S_CHECK(timeline.shown(now, pose) != nullptr);
    if (now > 10.0) {
      slowest = std::min(slowest, double(pose.position.x) - previous);
      KCD2S_CHECK(double(pose.position.x) > previous);
    }
    previous = pose.position.x;
  }
  // A frame's walk at 6 m/s is 0.2 m: never a stop.
  KCD2S_CHECK(slowest > 0.15);
  KCD2S_CHECK(timeline.delay() > 1.0);
  KCD2S_CHECK(timeline.sample_count() > 1);
}

// samples that come in uneven bursts, later than the delay covers. Walking at 2 m/s, no
// 60 Hz frame may show a step of a body twice as fast.
KCD2S_TEST(replica_timeline, a_late_sample_is_caught_up_and_not_a_jump) {
  ReplicaTimeline<int> timeline(kTeleportMeters);
  constexpr double kWalk = 2.0;
  constexpr double kFrame = 1.0 / 60.0;
  std::mt19937 random(487);
  std::uniform_real_distribution<double> gap(0.05, 0.9);
  double next_sample = 0.0;
  float last_x = 0.0f;
  float largest_step = 0.0f;
  bool started = false;
  int value = 0;
  for (double now = 0.0; now < 60.0; now += kFrame) {
    if (now >= next_sample) {
      TimelinePose pose;
      pose.position.x = float(now * kWalk);
      timeline.add(std::int64_t(now * 1000.0), ++value, pose, now);
      next_sample = now + gap(random);
    }
    TimelinePose shown;
    if (!timeline.shown(now, shown)) {
      continue;
    }
    if (started && now > 3.0) {
      largest_step = std::max(largest_step, std::fabs(shown.position.x - last_x));
    }
    started = true;
    last_x = shown.position.x;
  }
  KCD2S_CHECK(largest_step < float(2.0 * kWalk * kFrame) + 0.03f);
}

// a walking body with an owner hop of 0.4 m between two samples close together: the
// pair claims 40 m/s, the body is still a walker and no frame shows the hop.
KCD2S_TEST(replica_timeline, a_hop_between_close_samples_is_not_the_bodys_pace) {
  ReplicaTimeline<int> timeline(kTeleportMeters);
  constexpr double kWalk = 2.0;
  constexpr double kFrame = 1.0 / 60.0;
  constexpr double kSampleEvery = 0.1;
  double next_sample = 0.0;
  float last_x = 0.0f;
  float largest_step = 0.0f;
  bool started = false;
  bool hopped = false;
  float hop = 0.0f;
  int value = 0;
  for (double now = 0.0; now < 12.0; now += kFrame) {
    if (now >= next_sample) {
      if (now > 6.0 && !hopped) {
        hopped = true;
        TimelinePose before;
        before.position.x = float(now * kWalk);
        timeline.add(std::int64_t(now * 1000.0), ++value, before, now);
        hop = 0.4f;
        TimelinePose after;
        after.position.x = float(now * kWalk) + hop;
        timeline.add(std::int64_t(now * 1000.0) + 10, ++value, after, now);
      } else {
        TimelinePose pose;
        pose.position.x = float(now * kWalk) + hop;
        timeline.add(std::int64_t(now * 1000.0), ++value, pose, now);
      }
      next_sample = now + kSampleEvery;
    }
    TimelinePose shown;
    if (!timeline.shown(now, shown)) {
      continue;
    }
    if (started && now > 3.0) {
      largest_step = std::max(largest_step, std::fabs(shown.position.x - last_x));
    }
    started = true;
    last_x = shown.position.x;
  }
  KCD2S_CHECK(hopped);
  KCD2S_CHECK(largest_step < 0.15f);
}

KCD2S_TEST(replica_timeline, a_jump_faster_than_a_body_is_still_a_teleport) {
  ReplicaTimeline<int> timeline(5.0f);
  const Quat still{0.0f, 0.0f, 0.0f, 1.0f};
  timeline.add(1000, 1, {{0.0f, 0.0f, 0.0f}, still}, 1.0);
  timeline.add(1100, 2, {{100.0f, 0.0f, 0.0f}, still}, 1.1);
  KCD2S_CHECK_EQ(timeline.sample_count(), std::size_t{1});
}
