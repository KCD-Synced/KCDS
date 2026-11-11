// Portable load-measurement helpers used by kcd2s-server (opt-in metrics) and kcd2s-load-probe.
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "../local/load/load_options.hpp"
#include "../local/load/markers.hpp"
#include "../local/load/reference_pose.hpp"
#include "../local/load/workload.hpp"
#include "../local/metrics/counters.hpp"
#include "../local/metrics/duration_histogram.hpp"
#include "../local/metrics/json_object.hpp"
#include "../local/metrics/metrics_settings.hpp"
#include "../local/netsim/impairment.hpp"
#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/server.hpp"
#include "kcd2s/testkit.hpp"

using namespace kcd2s;
using namespace kcd2s::local;

namespace {

bool near(double actual, double expected, double tolerance) {
  return std::abs(actual - expected) <= tolerance;
}

bool rejects(const std::vector<std::string>& arguments) {
  try {
    load::parse_load_options(arguments);
  } catch (const std::invalid_argument&) {
    return true;
  }
  return false;
}

replication::Mirror::Entity reference_state() {
  const auto schema = player_schema();
  replication::World world(schema);
  world.set_tick(1);
  const auto player = spawn_player(world);
  world.write(player, Position, core::Vec3{1647.5f, 2155.5f, 35.4f});
  world.write(player, PoseValid, true);
  world.write(player, MotionValid, true);
  world.write(player, MotionMask, std::int64_t{0x12345});
  world.write(player, MotionClock, std::int64_t{123456});
  world.write(player, MotionParams0, core::Vec3{1, 2, 3});
  integration::PlayerPosture posture;
  posture.guard = true;
  world.write(player, Posture, integration::posture_field(posture));
  world.write(player, BodyVisible, false);
  world.write(player, AimEnabled, true);
  world.write(player, HeldProp, std::int64_t{1});
  world.write(player, Outfit, std::string("KO2") + std::string(19, '\x01'));
  world.write(player, Appearance, std::string("male|body|head|hair|beard"));
  AnimationState animation;
  animation[0].sequence = 9;
  animation[0].clip = 321;
  world.write(player, Animations, encode_animation(animation));
  replication::ClientRepl client;
  replication::Mirror mirror(schema);
  mirror.apply(replication::build_snapshot(world, 1, {player}, client));
  return *mirror.get(player);
}

replication::Mirror::Entity idle_reference_state() {
  auto state = reference_state();
  state.fields[Posture] = integration::posture_field({});
  state.fields[BodyVisible] = true;
  state.fields[AimEnabled] = false;
  state.fields[HeldProp] = std::int64_t{0};
  state.fields[MotionClock] = std::int64_t{1000};
  return state;
}

struct CaptureFile {
  std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("kcd2s-reference-capture-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::path file = directory / "native-animation.bin";

  CaptureFile() {
    if (!std::filesystem::create_directory(directory)) {
      throw std::runtime_error("could not create unique reference capture test directory");
    }
  }
  ~CaptureFile() {
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
    std::filesystem::remove(directory, ignored);
  }
  std::string read() const {
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }
};

const replication::FieldValue* input_field(const load::ReferenceInput& input, core::FieldId field) {
  for (const auto& [id, value] : input) {
    if (id == field) {
      return &value;
    }
  }
  return nullptr;
}

}  // namespace

KCD2S_TEST(load_metrics, empty_histogram_reports_zero) {
  metrics::DurationHistogram histogram;
  KCD2S_CHECK_EQ(histogram.count(), std::uint64_t{0});
  KCD2S_CHECK_EQ(histogram.percentile(50), 0.0);
  KCD2S_CHECK_EQ(histogram.percentile(99), 0.0);
  KCD2S_CHECK_EQ(histogram.max(), 0.0);
  KCD2S_CHECK_EQ(histogram.mean(), 0.0);
}

KCD2S_TEST(load_metrics, nearest_rank_percentiles_within_bucket_resolution) {
  metrics::DurationHistogram histogram;
  for (int i = 1; i <= 100; ++i) {
    histogram.add(static_cast<double>(i));
  }
  KCD2S_CHECK_EQ(histogram.count(), std::uint64_t{100});
  // 1..100 ms fall in 100 us buckets: a percentile may exceed its sample by at most 0.1 ms.
  KCD2S_CHECK(histogram.percentile(50) >= 50.0 && histogram.percentile(50) <= 50.1);
  KCD2S_CHECK(histogram.percentile(95) >= 95.0 && histogram.percentile(95) <= 95.1);
  KCD2S_CHECK(histogram.percentile(99) >= 99.0 && histogram.percentile(99) <= 99.1);
  KCD2S_CHECK_EQ(histogram.percentile(100), 100.0);
  KCD2S_CHECK_EQ(histogram.max(), 100.0);
  KCD2S_CHECK(near(histogram.mean(), 50.5, 1e-9));
}

KCD2S_TEST(load_metrics, single_sample_is_clamped_to_exact_maximum) {
  metrics::DurationHistogram histogram;
  histogram.add(0.0042);
  KCD2S_CHECK_EQ(histogram.percentile(50), 0.0042);
  KCD2S_CHECK_EQ(histogram.percentile(99), 0.0042);
}

KCD2S_TEST(load_metrics, fine_buckets_separate_sub_millisecond_ticks) {
  metrics::DurationHistogram histogram;
  for (int i = 0; i < 99; ++i) {
    histogram.add(0.105);
  }
  histogram.add(0.9);
  KCD2S_CHECK(histogram.percentile(50) >= 0.105 && histogram.percentile(50) <= 0.115);
  KCD2S_CHECK(histogram.percentile(99) >= 0.105 && histogram.percentile(99) <= 0.115);
  KCD2S_CHECK_EQ(histogram.percentile(100), 0.9);
}

KCD2S_TEST(load_metrics, overflow_and_invalid_samples) {
  metrics::DurationHistogram histogram;
  histogram.add(-5.0);
  histogram.add(std::numeric_limits<double>::quiet_NaN());
  histogram.add(25000.0);
  KCD2S_CHECK_EQ(histogram.count(), std::uint64_t{3});
  KCD2S_CHECK_EQ(histogram.percentile(50), 0.01);
  KCD2S_CHECK_EQ(histogram.percentile(100), 25000.0);
  KCD2S_CHECK_EQ(histogram.max(), 25000.0);
}

KCD2S_TEST(load_metrics, bucket_bounds_are_monotonic_across_regions) {
  using metrics::DurationHistogram;
  KCD2S_CHECK_EQ(DurationHistogram::bucket_index(0.0), std::size_t{0});
  KCD2S_CHECK(near(DurationHistogram::bucket_upper_ms(0), 0.01, 1e-12));
  const double samples[] = {0.0, 0.009, 0.5, 0.999, 1.0, 1.05, 16.0, 99.9, 100.0, 5000.0, 9999.0};
  double previous_upper = 0.0;
  for (double sample : samples) {
    const auto index = DurationHistogram::bucket_index(sample);
    const double upper = DurationHistogram::bucket_upper_ms(index);
    KCD2S_CHECK(upper > sample);
    KCD2S_CHECK(upper >= previous_upper);
    previous_upper = upper;
  }
  KCD2S_CHECK(
      std::isinf(DurationHistogram::bucket_upper_ms(DurationHistogram::bucket_index(20000.0))));
}

KCD2S_TEST(load_metrics, merge_and_reset) {
  metrics::DurationHistogram first;
  metrics::DurationHistogram second;
  first.add(1.0);
  first.add(2.0);
  second.add(30.0);
  first.merge(second);
  KCD2S_CHECK_EQ(first.count(), std::uint64_t{3});
  KCD2S_CHECK_EQ(first.max(), 30.0);
  KCD2S_CHECK(near(first.mean(), 11.0, 1e-9));
  first.reset();
  KCD2S_CHECK_EQ(first.count(), std::uint64_t{0});
  KCD2S_CHECK_EQ(first.max(), 0.0);
}

KCD2S_TEST(load_metrics, traffic_counters_and_delta) {
  metrics::TrafficCounters counters;
  counters.record_sent(100);
  counters.record_sent(50);
  counters.record_received(1200);
  counters.record_would_block();
  const metrics::TrafficCounters earlier = counters;
  counters.record_sent(10);
  counters.record_received(20);
  counters.record_dropped();
  counters.record_dropped();
  KCD2S_CHECK_EQ(counters.send_dropped, std::uint64_t{2});
  KCD2S_CHECK_EQ(counters.packets_sent, std::uint64_t{3});
  KCD2S_CHECK_EQ(counters.bytes_sent, std::uint64_t{160});
  KCD2S_CHECK_EQ(counters.packets_received, std::uint64_t{2});
  KCD2S_CHECK_EQ(counters.bytes_received, std::uint64_t{1220});
  const auto delta = metrics::traffic_delta(counters, earlier);
  KCD2S_CHECK_EQ(delta.packets_sent, std::uint64_t{1});
  KCD2S_CHECK_EQ(delta.bytes_sent, std::uint64_t{10});
  KCD2S_CHECK_EQ(delta.packets_received, std::uint64_t{1});
  KCD2S_CHECK_EQ(delta.bytes_received, std::uint64_t{20});
  KCD2S_CHECK_EQ(delta.send_would_block, std::uint64_t{0});
  KCD2S_CHECK_EQ(delta.send_dropped, std::uint64_t{2});
}

KCD2S_TEST(load_metrics, json_object_escapes_and_nests) {
  metrics::JsonObject inner;
  inner.add_count("n", 7);
  metrics::JsonObject object;
  object.add_text("text", "a\"b\\c\n\x01");
  object.add_integer("negative", -3);
  object.add_number("ratio", 0.25);
  object.add_number("invalid", std::numeric_limits<double>::infinity());
  object.add_flag("on", true);
  object.add_object("inner", inner);
  KCD2S_CHECK_EQ(object.str(),
                 std::string("{\"text\":\"a\\\"b\\\\c\\n\\u0001\",\"negative\":-3,\"ratio\":0.25,"
                             "\"invalid\":null,\"on\":true,\"inner\":{\"n\":7}}"));
  KCD2S_CHECK_EQ(metrics::JsonObject().str(), std::string("{}"));
}

KCD2S_TEST(load_metrics, histogram_description_fields) {
  metrics::DurationHistogram histogram;
  histogram.add(2.0);
  const std::string text = metrics::describe(histogram).str();
  KCD2S_CHECK_EQ(text, std::string("{\"count\":1,\"mean_ms\":2,\"p50_ms\":2,\"p95_ms\":2,"
                                   "\"p99_ms\":2,\"max_ms\":2}"));
}

KCD2S_TEST(load_metrics, metrics_interval_is_opt_in) {
  KCD2S_CHECK(!metrics::parse_metrics_interval(nullptr));
  KCD2S_CHECK(!metrics::parse_metrics_interval(""));
  KCD2S_CHECK(!metrics::parse_metrics_interval("0"));
  KCD2S_CHECK(!metrics::parse_metrics_interval("-1"));
  KCD2S_CHECK(!metrics::parse_metrics_interval("off"));
  KCD2S_CHECK(!metrics::parse_metrics_interval("1s"));
  KCD2S_CHECK(!metrics::parse_metrics_interval("7200"));
  // Keep the optionals alive: KCD2S_CHECK_EQ binds its operands by reference.
  const auto one_second = metrics::parse_metrics_interval("1");
  const auto half_second = metrics::parse_metrics_interval("0.5");
  KCD2S_CHECK(one_second.has_value());
  KCD2S_CHECK(half_second.has_value());
  KCD2S_CHECK_EQ(one_second.value_or(0.0), 1.0);
  KCD2S_CHECK_EQ(half_second.value_or(0.0), 0.5);
}

KCD2S_TEST(load_metrics, probe_options_defaults_and_overrides) {
  const auto defaults = load::parse_load_options({});
  KCD2S_CHECK_EQ(defaults.loss_fraction, 0.0);
  KCD2S_CHECK_EQ(defaults.delay_ms, 0.0);
  KCD2S_CHECK(defaults.leave == load::LeaveMode::Timeout);
  KCD2S_CHECK_EQ(defaults.clients, 8u);
  KCD2S_CHECK_EQ(defaults.port, 7777u);
  KCD2S_CHECK(defaults.workload == load::Workload::Distributed);
  KCD2S_CHECK(defaults.input_fields == load::InputFields::Full);
  const auto options = load::parse_load_options(
      {"--clients", "100", "--workload", "dense", "--duration", "60", "--port", "7801", "--tick-hz",
       "30", "--input-hz", "15", "--input-fields", "compact", "--host", "127.0.0.2"});
  KCD2S_CHECK_EQ(options.clients, 100u);
  KCD2S_CHECK(options.workload == load::Workload::Dense);
  KCD2S_CHECK_EQ(options.duration_seconds, 60.0);
  KCD2S_CHECK_EQ(options.port, 7801u);
  KCD2S_CHECK_EQ(options.tick_hz, 30.0);
  KCD2S_CHECK_EQ(options.input_hz, 15.0);
  KCD2S_CHECK(options.input_fields == load::InputFields::Compact);
  KCD2S_CHECK_EQ(options.host, std::string("127.0.0.2"));
  KCD2S_CHECK_EQ(std::string(load::workload_name(options.workload)), std::string("dense"));
}

KCD2S_TEST(load_metrics, probe_options_reject_malformed_input) {
  KCD2S_CHECK(rejects({"--clients"}));
  KCD2S_CHECK(rejects({"--clients", "0"}));
  KCD2S_CHECK(rejects({"--clients", "2.5"}));
  KCD2S_CHECK(rejects({"--clients", "ten"}));
  KCD2S_CHECK(rejects({"--port", "70000"}));
  KCD2S_CHECK(rejects({"--workload", "crowd"}));
  KCD2S_CHECK(rejects({"--input-fields", "all"}));
  KCD2S_CHECK(rejects({"--unknown", "1"}));
  KCD2S_CHECK(rejects({"--tick-hz", "20", "--input-hz", "40"}));
}

KCD2S_TEST(load_metrics, reference_player_is_opt_in_and_requires_a_valid_id_and_full_fields) {
  KCD2S_CHECK_EQ(load::parse_load_options({}).reference_player, 0u);
  KCD2S_CHECK_EQ(load::parse_load_options({"--reference-player", "42"}).reference_player, 42u);
  KCD2S_CHECK_EQ(load::parse_load_options({"--reference-player", "4294967295"}).reference_player,
                 std::numeric_limits<std::uint32_t>::max());
  for (const auto* invalid : {"0", "-1", "1.5", "missing", "4294967296"}) {
    KCD2S_CHECK(rejects({"--reference-player", invalid}));
  }
  KCD2S_CHECK(rejects({"--reference-player", "1", "--input-fields", "compact"}));
  KCD2S_CHECK(rejects({"--input-fields", "compact", "--reference-player", "1"}));
  KCD2S_CHECK(load::parse_load_options({}).reference_layout == load::ReferenceLayout::Ring);
  KCD2S_CHECK(load::parse_load_options({"--reference-layout", "ring"}).reference_layout ==
              load::ReferenceLayout::Ring);
  KCD2S_CHECK(load::parse_load_options({"--reference-layout", "spawn", "--reference-player", "1"})
                  .reference_layout == load::ReferenceLayout::Spawn);
  KCD2S_CHECK(rejects({"--reference-layout", "spawn"}));
  KCD2S_CHECK(load::parse_load_options({"--reference-layout", "spawn-follow", "--reference-player",
                                        "1", "--workload", "idle"})
                  .reference_layout == load::ReferenceLayout::SpawnFollow);
  KCD2S_CHECK(rejects({"--reference-layout", "spawn-follow"}));
  KCD2S_CHECK(rejects({"--reference-layout", "spawn-follow", "--reference-player", "1",
                       "--reference-updates", "frozen-idle", "--workload", "idle"}));
  KCD2S_CHECK(rejects({"--reference-player", "1", "--reference-layout", "grid"}));
}

KCD2S_TEST(load_metrics, reference_spawn_keeps_first_authoritative_position_without_feedback) {
  auto own = reference_state();
  const core::Vec3 terrain_spawn{1700, 2222, 47.125f};
  own.fields[Position] = terrain_spawn;
  load::ReferenceSpawnPosition captured;
  KCD2S_CHECK(captured.capture(&own));
  own.fields[Position] = core::Vec3{1, 2, 3};  // later echo or movement is never a new spawn
  KCD2S_CHECK(captured.capture(&own));
  KCD2S_CHECK(*captured.get() == terrain_spawn);

  const auto state = reference_state();
  auto reference = load::read_reference_pose(&state);
  KCD2S_CHECK(reference.has_value());
  if (!reference) {
    return;
  }
  const auto options = load::parse_load_options(
      {"--reference-player", "1", "--reference-layout", "spawn", "--clients", "4"});
  const auto first = load::reference_input(*reference, options, 0, 200000, captured.get());
  reference->position = {3000, 4000, 5000};
  const auto later = load::reference_input(*reference, options, 3, 200100, captured.get());
  KCD2S_CHECK(std::get<core::Vec3>(*input_field(first, Position)) == terrain_spawn);
  KCD2S_CHECK(*input_field(first, Position) == *input_field(later, Position));
  KCD2S_CHECK(*input_field(first, Animations) == state.fields[Animations]);
  KCD2S_CHECK(*input_field(first, Outfit) == state.fields[Outfit]);
  KCD2S_CHECK(load::reference_input(*reference, options, 0, 200200).empty());
  KCD2S_CHECK(load::reference_input(*reference, options, 0, 200200,
                                    core::Vec3{0, 0, std::numeric_limits<float>::infinity()})
                  .empty());

  auto ring = options;
  ring.reference_layout = load::ReferenceLayout::Ring;
  const auto plain_ring = load::reference_input(*reference, ring, 0, 200300);
  const auto ignored_spawn = load::reference_input(*reference, ring, 0, 200300, captured.get());
  KCD2S_CHECK(plain_ring == ignored_spawn);
}

KCD2S_TEST(load_metrics, reference_follow_moves_and_stops_without_reusing_owner_echoes) {
  auto own = reference_state();
  own.epoch = 3;
  const core::Vec3 spawn{1700, 2200, 40};
  own.fields[Position] = spawn;
  auto state = reference_state();
  state.fields[Position] = core::Vec3{1600, 2100, 30};
  state.fields[LookTarget] = core::Vec3{1604, 2105, 32};
  state.fields[AimTarget] = core::Vec3{1610, 2110, 34};
  const auto parsed = load::read_reference_pose(&state);
  KCD2S_CHECK(parsed.has_value());
  if (!parsed)
    return;
  auto source = *parsed;
  load::ReferenceFollowPosition follow;
  KCD2S_CHECK(follow.observe(source, 99, 1, 3, &own, spawn) == spawn);
  source.position = {1602, 2097, 31};
  ++source.owner_clock_ms;
  own.fields[Position] = core::Vec3{1, 2, 3};  // an owner echo must never redefine the anchor
  const core::Vec3 moved{1702, 2197, 41};
  KCD2S_CHECK(follow.observe(source, 99, 1, 3, &own, spawn) == moved);
  ++source.owner_clock_ms;
  KCD2S_CHECK(follow.observe(source, 99, 1, 3, &own, spawn) == moved);  // stop
  source.position.y += 2;
  ++source.owner_clock_ms;
  const core::Vec3 strafed{1702, 2199, 41};
  KCD2S_CHECK(follow.observe(source, 99, 1, 3, &own, spawn) == strafed);
  const auto options =
      load::parse_load_options({"--reference-player", "1", "--reference-layout", "spawn-follow"});
  const auto input = load::reference_input(source, options, 0, 200000, spawn, strafed);
  KCD2S_CHECK(!input.empty());
  if (input.empty())
    return;
  KCD2S_CHECK(std::get<core::Vec3>(*input_field(input, Position)) == strafed);
  KCD2S_CHECK(
      (std::get<core::Vec3>(*input_field(input, LookTarget)) == core::Vec3{1704, 2205, 42}));
  KCD2S_CHECK((std::get<core::Vec3>(*input_field(input, AimTarget)) == core::Vec3{1710, 2210, 44}));
  for (const auto field :
       {Animations, Rotation, Velocity, MotionParams0, MotionMask, LookDirection, LookTuning}) {
    KCD2S_CHECK(*input_field(input, field) == state.fields[field]);
  }
  KCD2S_CHECK(load::reference_input(source, options, 0, 200100, spawn).empty());
  KCD2S_CHECK(load::reference_input(source, options, 0, 200100, spawn,
                                    core::Vec3{0, 0, std::numeric_limits<float>::infinity()})
                  .empty());
  for (auto& [field, value] : source.fields) {
    if (field == LookTarget)
      value = core::Vec3{1000000, 0, 0};
  }
  KCD2S_CHECK(load::reference_input(source, options, 0, 200100, spawn, strafed).empty());
}

KCD2S_TEST(load_metrics, reference_follow_rebases_forces_and_waits_for_unexplained_epochs) {
  auto own = reference_state();
  own.epoch = 3;
  const core::Vec3 spawn{100, 200, 30};
  own.fields[Position] = spawn;
  load::ReferencePose source;
  source.position = {10, 20, 3};
  source.owner_clock_ms = 1000;
  load::ReferenceFollowPosition follow;
  KCD2S_CHECK(follow.observe(source, 99, 1, 3, &own, spawn) == spawn);
  source.position.x += 5;
  ++source.owner_clock_ms;
  KCD2S_CHECK((follow.observe(source, 99, 1, 3, &own, spawn) == core::Vec3{105, 200, 30}));
  integration::ClientPeer::ForcedTransform force;
  force.entity = 99;
  force.revision = 1;
  force.epoch = 4;
  force.position = {400, 500, 60};
  follow.force(force);
  // A drained authoritative force is usable before the mirror's old epoch catches up.
  KCD2S_CHECK(follow.observe(source, 99, 1, 4, &own, spawn) == force.position);
  source.position.x += 2;
  ++source.owner_clock_ms;
  KCD2S_CHECK((follow.observe(source, 99, 1, 4, &own, spawn) == core::Vec3{402, 500, 60}));
  force.epoch = 5;
  force.has_position = false;
  force.has_rotation = true;
  follow.force(force);
  KCD2S_CHECK((follow.observe(source, 99, 1, 5, &own, spawn) == core::Vec3{402, 500, 60}));
  source.position.y += 2;
  ++source.owner_clock_ms;
  KCD2S_CHECK((follow.observe(source, 99, 1, 5, &own, spawn) == core::Vec3{402, 502, 60}));
  KCD2S_CHECK(!follow.observe(source, 99, 1, 6, &own, spawn));
  own.epoch = 6;
  own.fields[Position] = core::Vec3{600, 700, 80};
  KCD2S_CHECK((follow.observe(source, 99, 1, 6, &own, spawn) == core::Vec3{600, 700, 80}));
  KCD2S_CHECK(!follow.observe(source, 99, 1, std::nullopt, &own, spawn));
}

KCD2S_TEST(load_metrics, reference_follow_resets_source_scope_clock_and_avatar_identity) {
  auto own = reference_state();
  own.epoch = 1;
  const core::Vec3 spawn{100, 200, 30};
  own.fields[Position] = spawn;
  load::ReferencePose source;
  source.position = {10, 20, 3};
  source.owner_clock_ms = 1000;
  source.epoch = 255;
  load::ReferenceFollowPosition follow;
  KCD2S_CHECK(follow.observe(source, 99, 1, 1, &own, spawn) == spawn);
  source.position.x += 5;
  ++source.owner_clock_ms;
  const core::Vec3 moved{105, 200, 30};
  KCD2S_CHECK(follow.observe(source, 99, 1, 1, &own, spawn) == moved);
  source.epoch = 0;  // valid wrapping epoch; teleport must not translate the crowd
  source.position = {900, 900, 900};
  KCD2S_CHECK(follow.observe(source, 99, 1, 1, &own, spawn) == moved);
  source.epoch = 255;  // an older source snapshot is ignored
  KCD2S_CHECK(!follow.observe(source, 99, 1, 1, &own, spawn));
  source.epoch = 0;
  source.owner_clock_ms = 5;  // reset owner clock is a new origin
  source.position = {800, 800, 800};
  KCD2S_CHECK(follow.observe(source, 99, 1, 1, &own, spawn) == moved);
  follow.source_missing();
  source.position = {700, 700, 700};
  source.owner_clock_ms = 6;
  KCD2S_CHECK(follow.observe(source, 99, 1, 1, &own, spawn) == moved);
  const core::Vec3 new_spawn{400, 500, 60};
  own.fields[Position] = new_spawn;
  KCD2S_CHECK(follow.observe(source, 99, 2, 1, &own, new_spawn) == new_spawn);
  KCD2S_CHECK(follow.observe(source, 100, 2, 1, &own, spawn) == spawn);
  follow = {};  // reconnect with a reused ID also discards the old origin
  KCD2S_CHECK(follow.observe(source, 100, 2, 1, &own, new_spawn) == new_spawn);
  source.position.z = std::numeric_limits<float>::quiet_NaN();
  KCD2S_CHECK(!follow.observe(source, 100, 2, 1, &own, new_spawn));
}

KCD2S_TEST(load_metrics, frozen_reference_options_are_explicit_and_idle_only) {
  const auto defaults = load::parse_load_options({});
  KCD2S_CHECK(defaults.reference_updates == load::ReferenceUpdates::Live);
  KCD2S_CHECK(defaults.reference_capture.empty());
  const auto options = load::parse_load_options({"--reference-player", "1", "--workload", "idle",
                                                 "--reference-updates", "frozen-idle",
                                                 "--reference-capture", "exact-native-body.bin"});
  KCD2S_CHECK(options.reference_updates == load::ReferenceUpdates::FrozenIdle);
  KCD2S_CHECK_EQ(options.reference_capture, std::string("exact-native-body.bin"));
  KCD2S_CHECK(rejects({"--reference-updates", "freeze"}));
  KCD2S_CHECK(rejects({"--reference-updates", "frozen-idle", "--workload", "idle"}));
  KCD2S_CHECK(rejects({"--reference-player", "1", "--reference-updates", "frozen-idle"}));
  KCD2S_CHECK(rejects({"--reference-capture", "body.bin"}));
  KCD2S_CHECK(rejects({"--reference-player", "1", "--reference-capture", ""}));
}

KCD2S_TEST(load_metrics, frozen_idle_requires_confirmed_stability_and_preserves_current_progress) {
  load::ReferenceSource source(load::ReferenceUpdates::FrozenIdle);
  auto state = reference_state();  // guarding, aiming and holding a prop is not idle
  KCD2S_CHECK(!source.observe(&state, 1, 0));
  state = idle_reference_state();
  KCD2S_CHECK(integration::PlayerInputPublication::idle(state.fields));
  KCD2S_CHECK(!source.observe(&state, 1, 1));
  KCD2S_CHECK(!source.observe(&state, 1, 10));  // one stale snapshot cannot confirm stability
  state.fields[MotionClock] = std::int64_t{1499};
  KCD2S_CHECK(!source.observe(&state, 1, 10.1));
  auto clips = decode_animation(std::get<std::string>(state.fields[Animations]));
  KCD2S_CHECK(clips.has_value());
  if (!clips) {
    return;
  }
  (*clips)[0].params.normalized_time = 0.75f;
  state.fields[Animations] = encode_animation(*clips);
  state.fields[MotionClock] = std::int64_t{1500};
  const auto* frozen = source.observe(&state, 1, 10.2);
  KCD2S_CHECK(frozen != nullptr);
  KCD2S_CHECK(source.frozen());
  if (!frozen) {
    return;
  }
  const auto options = load::parse_load_options({"--reference-player", "1"});
  const auto input = load::reference_input(*frozen, options, 0, 2000);
  KCD2S_CHECK(*input_field(input, Animations) == state.fields[Animations]);
  KCD2S_CHECK(*input_field(input, Outfit) == state.fields[Outfit]);
}

KCD2S_TEST(load_metrics, frozen_idle_restarts_settling_on_activity_start_or_parameter_change) {
  load::ReferenceSource source(load::ReferenceUpdates::FrozenIdle);
  auto state = idle_reference_state();
  KCD2S_CHECK(!source.observe(&state, 1, 0));
  state.fields[MotionClock] = std::int64_t{1500};
  state.fields[Velocity] = core::Vec3{1, 0, 0};
  KCD2S_CHECK(!source.observe(&state, 1, 0.5));
  state.fields[Velocity] = core::Vec3{};
  KCD2S_CHECK(!source.observe(&state, 1, 0.6));
  auto clips = decode_animation(std::get<std::string>(state.fields[Animations]));
  KCD2S_CHECK(clips.has_value());
  if (!clips) {
    return;
  }
  (*clips)[0].sequence += 1;
  state.fields[Animations] = encode_animation(*clips);
  state.fields[MotionClock] = std::int64_t{2000};
  KCD2S_CHECK(!source.observe(&state, 1, 1.1));
  (*clips)[0].params.speed = 2;
  state.fields[Animations] = encode_animation(*clips);
  state.fields[MotionClock] = std::int64_t{2500};
  KCD2S_CHECK(!source.observe(&state, 1, 1.6));
  state.fields[MotionClock] = std::int64_t{3000};
  KCD2S_CHECK(!source.observe(&state, 1, 1.7));  // owner clock alone is insufficient
  KCD2S_CHECK(source.observe(&state, 1, 2.2) != nullptr);
}

KCD2S_TEST(load_metrics, frozen_reference_ignores_later_combat_but_keeps_owner_clock_and_forces) {
  load::ReferenceSource source(load::ReferenceUpdates::FrozenIdle);
  auto state = idle_reference_state();
  KCD2S_CHECK(!source.observe(&state, 7, 0));
  state.fields[MotionClock] = std::int64_t{1500};
  const auto* frozen = source.observe(&state, 7, 0.5);
  KCD2S_CHECK(frozen != nullptr);
  if (!frozen) {
    return;
  }
  const auto options =
      load::parse_load_options({"--reference-player", "1", "--reference-layout", "spawn",
                                "--reference-updates", "frozen-idle", "--workload", "idle"});
  load::ReferenceSpawnPosition spawn;
  KCD2S_CHECK(spawn.force_position({1700, 2100, 13.125f}));
  const auto before = load::reference_input(*frozen, options, 0, 2000, spawn.get());
  state = reference_state();
  state.fields[Position] = core::Vec3{3000, 4000, 50};
  const auto* after = source.observe(&state, 7, 3);
  KCD2S_CHECK(after != nullptr);
  if (!after) {
    return;
  }
  KCD2S_CHECK(spawn.force_position({1702, 2103, 14.875f}));
  const auto input = load::reference_input(*after, options, 0, 2500, spawn.get());
  KCD2S_CHECK(*input_field(input, Animations) == *input_field(before, Animations));
  KCD2S_CHECK(*input_field(input, Posture) == *input_field(before, Posture));
  KCD2S_CHECK_EQ(std::get<std::int64_t>(*input_field(input, MotionClock)), std::int64_t{2500});
  KCD2S_CHECK(std::get<core::Vec3>(*input_field(input, Position)) == *spawn.get());
  KCD2S_CHECK(source.observe(nullptr, 7, 4) != nullptr);
  KCD2S_CHECK(!source.observe(&state, 8, 5));  // a new native world must capture again
  KCD2S_CHECK(!source.frozen());
  state = idle_reference_state();
  KCD2S_CHECK(!source.observe(&state, 8, 6));
  state.fields[MotionClock] = std::int64_t{1500};
  KCD2S_CHECK(source.observe(&state, 8, 6.5) != nullptr);
}

KCD2S_TEST(load_metrics, live_reference_keeps_following_valid_active_updates) {
  load::ReferenceSource source(load::ReferenceUpdates::Live);
  auto state = reference_state();
  KCD2S_CHECK(source.observe(&state, 1, 0) != nullptr);
  state.fields[Position] = core::Vec3{3000, 4000, 50};
  const auto* current = source.observe(&state, 1, 0.1);
  KCD2S_CHECK(current != nullptr);
  if (current) {
    KCD2S_CHECK(current->position == std::get<core::Vec3>(state.fields[Position]));
  }
  KCD2S_CHECK(!source.frozen());
  KCD2S_CHECK(!source.observe(nullptr, 1, 0.2));
}

KCD2S_TEST(load_metrics, reference_capture_exports_first_valid_raw_binary_once) {
  CaptureFile files;
  load::ReferenceAnimationCapture capture(files.file.string());
  auto state = reference_state();
  const auto expected = std::get<std::string>(state.fields[Animations]);
  KCD2S_CHECK(expected.find('\0') != std::string::npos);
  state.fields[Animations] = std::string("invalid");
  KCD2S_CHECK(!capture.observe(&state));
  KCD2S_CHECK(!std::filesystem::exists(files.file));
  state.fields[Animations] = expected;
  KCD2S_CHECK(capture.observe(&state));
  KCD2S_CHECK(capture.completed());
  KCD2S_CHECK_EQ(capture.bytes(), expected.size());
  KCD2S_CHECK(files.read() == expected);
  state.fields[Animations] = encode_animation(AnimationState{});
  KCD2S_CHECK(!capture.observe(&state));
  KCD2S_CHECK(files.read() == expected);
}

KCD2S_TEST(load_metrics, reference_capture_write_failure_is_visible_and_not_completed) {
  CaptureFile files;
  load::ReferenceAnimationCapture capture(files.directory.string());  // cannot write a directory
  const auto state = reference_state();
  bool failed = false;
  try {
    capture.observe(&state);
  } catch (const std::runtime_error& error) {
    failed = std::string(error.what()).find("reference-capture:") != std::string::npos;
  }
  KCD2S_CHECK(failed);
  KCD2S_CHECK(!capture.completed());
  load::ReferenceAnimationCapture disabled("");
  KCD2S_CHECK(!disabled.observe(&state));
}

KCD2S_TEST(load_metrics,
           reference_diagnostics_count_semantic_changes_without_progress_or_payloads) {
  load::ReferenceDiagnostics diagnostics;
  auto state = idle_reference_state();
  KCD2S_CHECK(diagnostics.observe(&state, 1));
  KCD2S_CHECK(diagnostics.first_idle());
  KCD2S_CHECK(!diagnostics.observe(&state, 1));
  KCD2S_CHECK_EQ(diagnostics.observations(), std::uint64_t{1});
  auto clips = decode_animation(std::get<std::string>(state.fields[Animations]));
  KCD2S_CHECK(clips.has_value());
  if (!clips) {
    return;
  }
  (*clips)[0].params.normalized_time = 0.5f;
  state.fields[Animations] = encode_animation(*clips);
  state.fields[MotionClock] = std::int64_t{1100};
  KCD2S_CHECK(!diagnostics.observe(&state, 1));
  KCD2S_CHECK_EQ(diagnostics.observations(), std::uint64_t{2});
  KCD2S_CHECK_EQ(diagnostics.changes()[Animations], std::uint64_t{0});
  KCD2S_CHECK_EQ(diagnostics.changes()[MotionClock], std::uint64_t{0});
  state.fields[MotionClock] = std::int64_t{1200};
  state.fields[MotionParams0] = core::Vec3{9, 8, 7};
  state.fields[Velocity] = core::Vec3{1, 0, 0};
  KCD2S_CHECK(!diagnostics.observe(&state, 1));
  KCD2S_CHECK_EQ(diagnostics.idle_observations(), std::uint64_t{2});
  KCD2S_CHECK_EQ(diagnostics.changes()[MotionParams0], std::uint64_t{1});
  KCD2S_CHECK_EQ(diagnostics.changes()[Velocity], std::uint64_t{1});
  KCD2S_CHECK(
      !diagnostics.observe(&state, 2));  // reset comparison history, retain aggregate counts
  KCD2S_CHECK_EQ(diagnostics.changes()[Velocity], std::uint64_t{1});
}

KCD2S_TEST(load_metrics, reference_spawn_waits_for_a_valid_own_snapshot) {
  load::ReferenceSpawnPosition captured;
  KCD2S_CHECK(!captured.capture(nullptr));
  KCD2S_CHECK(!captured.get());
  auto state = reference_state();
  state.type = core::type_id("NetworkHorse");
  KCD2S_CHECK(!captured.capture(&state));
  state.type = core::type_id("NetworkPlayer");
  state.fields[Position] = false;
  KCD2S_CHECK(!captured.capture(&state));
  state.fields[Position] = core::Vec3{0, 0, std::numeric_limits<float>::quiet_NaN()};
  KCD2S_CHECK(!captured.capture(&state));
  state.fields[Position] = core::Vec3{4, 5, 6};
  KCD2S_CHECK(captured.capture(&state));
  KCD2S_CHECK((captured.get() == core::Vec3{4, 5, 6}));
}

KCD2S_TEST(load_metrics, reference_actor_applies_spawn_force_before_publishing_native_pose) {
  const auto schema = player_schema();
  integration::Server server(schema);
  auto& world = server.world();
  world.set_tick(1);
  const auto avatar = spawn_player(world);
  auto links = transport::make_loopback_pair();
  const auto owner = server.add_client(links.first.get(), avatar, 10000);
  server.set_owner(avatar, owner);
  integration::ClientPeer client(schema, links.second.get());
  double now = 0;
  auto pump = [&] {
    for (int i = 0; i < 4; ++i) {
      now += 1.0 / 60;
      world.set_tick(world.tick() + 1);
      server.tick(now);
      client.tick(now);
    }
  };
  pump();
  KCD2S_CHECK(client.owns(avatar));
  load::ReferenceSpawnPosition spawn;
  KCD2S_CHECK(spawn.capture(client.mirror().get(avatar)));
  const core::Vec3 terrain_spawn{1525, 2118, 13.625f};
  KCD2S_CHECK(server.force_transform(avatar, terrain_spawn));
  pump();

  const auto source = reference_state();
  const auto reference = load::read_reference_pose(&source);
  KCD2S_CHECK(reference.has_value());
  if (!reference) {
    return;
  }
  const auto options =
      load::parse_load_options({"--reference-player", "99", "--reference-layout", "spawn"});
  client.send_input(avatar, load::reference_input(*reference, options, 0, 200000, spawn.get()));
  pump();
  KCD2S_CHECK(!std::get<bool>(*world.read(avatar, PoseValid)));

  load::ReferenceFollowPosition follow;
  KCD2S_CHECK_EQ(load::adopt_reference_transforms(client, avatar, spawn, &follow), std::size_t{1});
  KCD2S_CHECK(*spawn.get() == terrain_spawn);
  const auto* own = client.mirror().get(avatar);
  KCD2S_CHECK(own != nullptr);
  if (own) {
    KCD2S_CHECK(follow.observe(*reference, avatar, 0, own->epoch, own, spawn.get()) ==
                terrain_spawn);
  }
  KCD2S_CHECK_EQ(load::adopt_reference_transforms(client, avatar, spawn), std::size_t{0});
  client.send_input(avatar, load::reference_input(*reference, options, 0, 200100, spawn.get()));
  pump();
  KCD2S_CHECK(std::get<bool>(*world.read(avatar, PoseValid)));
  KCD2S_CHECK(std::get<core::Vec3>(*world.read(avatar, Position)) == terrain_spawn);
  KCD2S_CHECK(spawn.capture(client.mirror().get(avatar)));
  KCD2S_CHECK(*spawn.get() == terrain_spawn);
}

KCD2S_TEST(load_metrics, reference_actor_adopts_force_queued_before_ownership_grant) {
  const auto schema = player_schema();
  integration::Server server(schema);
  auto& world = server.world();
  world.set_tick(1);
  const auto avatar = spawn_player(world);
  auto links = transport::make_loopback_pair();
  const auto owner = server.add_client(links.first.get(), avatar, 10000);
  integration::ClientPeer client(schema, links.second.get());
  double now = 0;
  auto pump = [&] {
    for (int i = 0; i < 4; ++i) {
      now += 1.0 / 60;
      world.set_tick(world.tick() + 1);
      server.tick(now);
      client.tick(now);
    }
  };
  pump();
  KCD2S_CHECK(client.mirror().get(avatar) != nullptr);
  KCD2S_CHECK(!client.owns(avatar));
  load::ReferenceSpawnPosition spawn;
  KCD2S_CHECK(spawn.capture(client.mirror().get(avatar)));
  const core::Vec3 terrain_spawn{1527, 2120, 14.875f};
  // The observer already knows this entity, so its force is queued before SetOwner.
  KCD2S_CHECK(server.force_transform(avatar, terrain_spawn));
  server.set_owner(avatar, owner);
  pump();
  KCD2S_CHECK(client.owns(avatar));
  KCD2S_CHECK_EQ(load::adopt_reference_transforms(client, avatar, spawn), std::size_t{1});
  KCD2S_CHECK(*spawn.get() == terrain_spawn);
  KCD2S_CHECK_EQ(load::adopt_reference_transforms(client, avatar, spawn), std::size_t{0});

  const auto source = reference_state();
  const auto reference = load::read_reference_pose(&source);
  KCD2S_CHECK(reference.has_value());
  if (!reference) {
    return;
  }
  const auto options =
      load::parse_load_options({"--reference-player", "99", "--reference-layout", "spawn"});
  client.send_input(avatar, load::reference_input(*reference, options, 0, 200000, spawn.get()));
  pump();
  KCD2S_CHECK(std::get<bool>(*world.read(avatar, PoseValid)));
  KCD2S_CHECK(std::get<core::Vec3>(*world.read(avatar, Position)) == terrain_spawn);
}

KCD2S_TEST(load_metrics,
           reference_actor_captures_forced_spawn_from_first_unknown_entity_construct) {
  const auto schema = player_schema();
  integration::Server server(schema);
  auto& world = server.world();
  world.set_tick(1);
  const auto avatar = spawn_player(world);
  auto links = transport::make_loopback_pair();
  const auto owner = server.add_client(links.first.get(), avatar, 10000);
  integration::ClientPeer client(schema, links.second.get());
  KCD2S_CHECK(client.mirror().get(avatar) == nullptr);
  KCD2S_CHECK(!client.owns(avatar));
  const core::Vec3 terrain_spawn{1530, 2123, 16.125f};
  // No construct or ownership grant has reached this peer. The server cannot send an
  // observer force for this unknown entity; its first construct must carry the final pose.
  KCD2S_CHECK(server.force_transform(avatar, terrain_spawn));
  server.set_owner(avatar, owner);
  double now = 0;
  auto pump = [&] {
    for (int i = 0; i < 4; ++i) {
      now += 1.0 / 60;
      world.set_tick(world.tick() + 1);
      server.tick(now);
      client.tick(now);
    }
  };
  pump();
  KCD2S_CHECK(client.owns(avatar));
  load::ReferenceSpawnPosition spawn;
  KCD2S_CHECK_EQ(load::adopt_reference_transforms(client, avatar, spawn), std::size_t{0});
  KCD2S_CHECK(spawn.capture(client.mirror().get(avatar)));
  KCD2S_CHECK(*spawn.get() == terrain_spawn);

  auto source = reference_state();
  auto reference = load::read_reference_pose(&source);
  KCD2S_CHECK(reference.has_value());
  if (!reference) {
    return;
  }
  const auto options =
      load::parse_load_options({"--reference-player", "99", "--reference-layout", "spawn"});
  client.send_input(avatar, load::reference_input(*reference, options, 0, 200000, spawn.get()));
  pump();
  KCD2S_CHECK(std::get<bool>(*world.read(avatar, PoseValid)));
  KCD2S_CHECK(std::get<core::Vec3>(*world.read(avatar, Position)) == terrain_spawn);
  reference->position = {3000, 4000, 5000};
  KCD2S_CHECK(spawn.capture(client.mirror().get(avatar)));
  client.send_input(avatar, load::reference_input(*reference, options, 0, 200100, spawn.get()));
  pump();
  KCD2S_CHECK(std::get<core::Vec3>(*world.read(avatar, Position)) == terrain_spawn);
}

KCD2S_TEST(load_metrics, reference_pose_keeps_native_animation_and_masks_on_a_stationary_ring) {
  const auto state = reference_state();
  const auto reference = load::read_reference_pose(&state);
  KCD2S_CHECK(reference.has_value());
  if (!reference) {
    return;
  }
  const auto options = load::parse_load_options(
      {"--reference-player", "1", "--clients", "4", "--dense-radius", "10", "--workload", "idle"});
  const auto first = load::reference_input(*reference, options, 0, 200000);
  const auto later = load::reference_input(*reference, options, 0, 200100);
  const auto opposite = load::reference_input(*reference, options, 2, 200000);
  for (const auto field : {Animations,
                           MotionMask,
                           MotionParams0,
                           MotionParams1,
                           MotionParams2,
                           MotionParams3,
                           MotionParams4,
                           MotionParams5,
                           Rotation,
                           MotionRotation,
                           MotionType,
                           MotionFlags,
                           Velocity,
                           MotionVelocity,
                           PoseValid,
                           MotionValid,
                           Posture,
                           BodyVisible,
                           AimEnabled,
                           HeldProp,
                           WeaponState,
                           UiBlocking,
                           VisorClosed,
                           LookEnabled,
                           LookValid,
                           LookTarget,
                           LookTuning,
                           LookDirection,
                           AimTarget,
                           Outfit,
                           Appearance,
                           BodyParts,
                           Disguise,
                           Hands,
                           WeaponAnimations,
                           HumanOverlayAnimation,
                           RagdollPose,
                           Seats,
                           Carry,
                           StationAction,
                           Grime,
                           AnimationProps}) {
    const auto* value = input_field(first, field);
    KCD2S_CHECK(value != nullptr);
    if (value) {
      KCD2S_CHECK(*value == state.fields[field]);
    }
  }
  const auto position = std::get<core::Vec3>(*input_field(first, Position));
  const auto other = std::get<core::Vec3>(*input_field(opposite, Position));
  KCD2S_CHECK(near(position.x, reference->position.x + 10, 0.001));
  KCD2S_CHECK(near(other.x, reference->position.x - 10, 0.001));
  KCD2S_CHECK_EQ(position.z, reference->position.z);
  KCD2S_CHECK(*input_field(first, Position) == *input_field(later, Position));
  KCD2S_CHECK_EQ(std::get<std::int64_t>(*input_field(first, MotionClock)), std::int64_t{200000});
  KCD2S_CHECK_EQ(std::get<std::int64_t>(*input_field(later, MotionClock)), std::int64_t{200100});
  KCD2S_CHECK_EQ(load::reference_marker_microseconds(200000), std::int64_t{200000000});
  KCD2S_CHECK_EQ(load::reference_marker_microseconds(std::numeric_limits<std::int64_t>::max()),
                 std::int64_t{0});
}

KCD2S_TEST(load_metrics, reference_pose_fails_closed_on_absent_or_invalid_native_state) {
  KCD2S_CHECK(!load::read_reference_pose(nullptr));
  const auto valid = reference_state();
  auto state = valid;
  state.type = core::type_id("NetworkNpc");
  KCD2S_CHECK(!load::read_reference_pose(&state));
  state = valid;
  state.fields.resize(MotionValid);
  KCD2S_CHECK(!load::read_reference_pose(&state));
  for (const auto field : {PoseValid, MotionValid}) {
    state = valid;
    state.fields[field] = false;
    KCD2S_CHECK(!load::read_reference_pose(&state));
  }
  state = valid;
  state.fields[Animations] = std::string{};
  KCD2S_CHECK(!load::read_reference_pose(&state));
  state = valid;
  state.fields[MotionParams3] = core::Vec3{std::numeric_limits<float>::quiet_NaN(), 0, 0};
  KCD2S_CHECK(!load::read_reference_pose(&state));
  state = valid;
  state.fields[MotionMask] = std::string("wrong type");
  KCD2S_CHECK(!load::read_reference_pose(&state));
  state = valid;
  state.fields[NativeRiderState] = std::string("mounted or malformed");
  KCD2S_CHECK(!load::read_reference_pose(&state));
  state = valid;
  state.fields[Rotation] = core::Quat{0, 0, 0, 0};
  KCD2S_CHECK(!load::read_reference_pose(&state));
  for (const auto field : {BodyVisible, AimEnabled, UiBlocking, VisorClosed}) {
    state = valid;
    state.fields[field] = std::int64_t{1};
    KCD2S_CHECK(!load::read_reference_pose(&state));
  }
  for (const auto field :
       {Outfit, Appearance, BodyParts, Disguise, Hands, WeaponAnimations, HumanOverlayAnimation,
        RagdollPose, Seats, Carry, StationAction, Grime, AnimationProps}) {
    state = valid;
    state.fields[field] = std::string("malformed\n");
    KCD2S_CHECK(!load::read_reference_pose(&state));
  }
  state = valid;
  state.fields[Posture] = std::int64_t{0};
  KCD2S_CHECK(!load::read_reference_pose(&state));
  state = valid;
  state.fields[HeldProp] = std::int64_t{4};
  KCD2S_CHECK(!load::read_reference_pose(&state));
  state = valid;
  state.fields[WeaponState] = std::int64_t{4};
  KCD2S_CHECK(load::read_reference_pose(&state));
  state.fields[WeaponState] = std::int64_t{7};
  KCD2S_CHECK(load::read_reference_pose(&state));
  state.fields[WeaponState] = std::int64_t{8};
  KCD2S_CHECK(!load::read_reference_pose(&state));
}

KCD2S_TEST(load_metrics, dense_workload_stays_inside_radius) {
  auto options = load::parse_load_options({"--workload", "dense", "--dense-radius", "10"});
  for (std::size_t i = 0; i < 100; ++i) {
    const auto path = load::circle_for(options, i, 100);
    for (double t : {0.0, 0.7, 13.3, 59.9}) {
      const auto sample = load::sample_movement(path, options.speed_meters_per_second, t);
      const double distance = std::hypot(sample.position.x, sample.position.y);
      KCD2S_CHECK(distance <= 10.0 + 1e-3);
      const double speed = std::hypot(sample.velocity.x, sample.velocity.y);
      KCD2S_CHECK(near(speed, options.speed_meters_per_second, 1e-3));
      const auto& q = sample.rotation;
      KCD2S_CHECK(near(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w, 1.0, 1e-5));
    }
  }
}

KCD2S_TEST(load_metrics, distributed_workload_separates_players) {
  auto options = load::parse_load_options({"--workload", "distributed", "--spacing", "200"});
  std::vector<core::Vec3> centres;
  for (std::size_t i = 0; i < 32; ++i) {
    centres.push_back(load::circle_for(options, i, 32).centre);
  }
  double closest = std::numeric_limits<double>::max();
  for (std::size_t a = 0; a < centres.size(); ++a) {
    for (std::size_t b = a + 1; b < centres.size(); ++b) {
      const double distance = std::hypot(centres[a].x - centres[b].x, centres[a].y - centres[b].y);
      if (distance < closest) {
        closest = distance;
      }
    }
  }
  KCD2S_CHECK(near(closest, 200.0, 1e-3));
}

KCD2S_TEST(load_metrics, server_reports_outstanding_reliable_messages) {
  replication::SchemaRegistry schema;
  auto type = schema.define("Player")
                  .field("position", replication::FieldKind::Vec3, replication::Priority::High,
                         replication::Authority::Owner)
                  .id();
  integration::Server server(schema);
  server.world().set_tick(1);
  KCD2S_CHECK_EQ(server.outstanding_reliable_total(), std::size_t{0});
  auto entity = server.world().spawn(type);
  server.world().write(entity, 0, core::Vec3{});
  auto links = transport::make_loopback_pair();
  auto peer = server.add_client(links.first.get(), entity, 10000);
  server.set_owner(entity, peer);
  KCD2S_CHECK(server.outstanding_reliable_total() >= std::size_t{1});
  integration::ClientPeer client(schema, links.second.get());
  double now = 0;
  for (int i = 0; i < 20; ++i) {
    now += 1.0 / 60;
    server.world().set_tick(server.world().tick() + 1);
    server.tick(now);
    client.tick(now);
  }
  KCD2S_CHECK(client.owns(entity));
  KCD2S_CHECK_EQ(server.outstanding_reliable_total(), std::size_t{0});
}

KCD2S_TEST(load_metrics, traffic_counters_track_largest_datagram) {
  metrics::TrafficCounters counters;
  counters.record_sent(300);
  counters.record_sent(1200);
  counters.record_sent(40);
  KCD2S_CHECK_EQ(counters.largest_sent, std::uint64_t{1200});
  const metrics::TrafficCounters earlier = counters;
  counters.record_sent(50);
  const metrics::TrafficCounters delta = metrics::traffic_delta(counters, earlier);
  KCD2S_CHECK_EQ(delta.largest_sent, std::uint64_t{1200});  // a running maximum, not a delta
}

KCD2S_TEST(load_metrics, histogram_buckets_are_lossless_for_merging) {
  metrics::DurationHistogram histogram;
  histogram.add(0.005);
  histogram.add(12.34);
  histogram.add(12.36);
  const std::string text = metrics::histogram_buckets(histogram).str();
  KCD2S_CHECK(text.find("\"count\":3") != std::string::npos);
  KCD2S_CHECK(text.find("\"0\":1") != std::string::npos);  // 5 us lands in the first bucket
  const std::size_t index = metrics::DurationHistogram::bucket_index(12.34);
  KCD2S_CHECK_EQ(histogram.bucket(index), std::uint64_t{2});
  const std::string entry = "\"" + std::to_string(index) + "\":2";
  KCD2S_CHECK(text.find(entry) != std::string::npos);
  KCD2S_CHECK_EQ(histogram.bucket(metrics::DurationHistogram::bucket_count()), std::uint64_t{0});
}

KCD2S_TEST(load_metrics, probe_options_split_one_layout_over_processes) {
  const load::LoadOptions first =
      load::parse_load_options({"--clients", "50", "--index-offset", "0", "--layout-count", "100"});
  const load::LoadOptions second = load::parse_load_options(
      {"--clients", "50", "--index-offset", "50", "--layout-count", "100"});
  KCD2S_CHECK_EQ(first.total_players(), 100u);
  KCD2S_CHECK_EQ(second.index_offset, 50u);
  // The same global index gets the same circle, whichever process drives it.
  const load::LoadOptions single = load::parse_load_options({"--clients", "100"});
  const load::CirclePath split = load::circle_for(second, second.index_offset + 7, 100);
  const load::CirclePath whole = load::circle_for(single, 57, 100);
  KCD2S_CHECK_EQ(split.centre.x, whole.centre.x);
  KCD2S_CHECK_EQ(split.centre.y, whole.centre.y);
  KCD2S_CHECK(rejects({"--clients", "60", "--index-offset", "50", "--layout-count", "100"}));
  KCD2S_CHECK(rejects({"--index-offset", "-1"}));
  const load::LoadOptions small = load::parse_load_options({"--clients", "4"});
  KCD2S_CHECK_EQ(small.total_players(), 4u);
}

KCD2S_TEST(load_metrics, convergence_markers_increase_and_age) {
  KCD2S_CHECK_EQ(load::next_marker(0, 1000), std::int64_t{1000});
  KCD2S_CHECK_EQ(load::next_marker(1000, 1000), std::int64_t{1001});  // never repeats
  KCD2S_CHECK_EQ(load::next_marker(5000, 1000), std::int64_t{5001});  // never goes back
  KCD2S_CHECK_EQ(load::marker_age_milliseconds(1000, 3500), 2.5);
  KCD2S_CHECK_EQ(load::marker_age_milliseconds(4000, 3500), 0.0);
  const std::int64_t a = load::marker_clock_microseconds();
  const std::int64_t b = load::marker_clock_microseconds();
  KCD2S_CHECK(b >= a);
}

KCD2S_TEST(load_metrics, probe_options_select_an_emulated_path_and_leave_mode) {
  const auto options =
      load::parse_load_options({"--loss", "0.05", "--delay-ms", "75", "--leave", "graceful"});
  KCD2S_CHECK_EQ(options.loss_fraction, 0.05);
  KCD2S_CHECK_EQ(options.delay_ms, 75.0);
  KCD2S_CHECK(options.leave == load::LeaveMode::Graceful);
  KCD2S_CHECK(rejects({"--loss", "0.6"}));
  KCD2S_CHECK(rejects({"--delay-ms", "-1"}));
  KCD2S_CHECK(rejects({"--leave", "crash"}));
}

KCD2S_TEST(load_metrics, impairment_delays_in_order_and_loses_the_requested_fraction) {
  netsim::ImpairmentSettings settings;
  settings.delay_seconds = 0.025;
  KCD2S_CHECK(settings.active());
  netsim::ImpairedQueue<int> delayed(settings);
  delayed.offer(1.0, 1);
  delayed.offer(1.001, 2);
  int item = 0;
  KCD2S_CHECK(!delayed.take_due(1.02, item));
  KCD2S_CHECK(delayed.take_due(1.026, item));
  KCD2S_CHECK_EQ(item, 1);
  KCD2S_CHECK(delayed.take_due(1.03, item));
  KCD2S_CHECK_EQ(item, 2);

  netsim::ImpairmentSettings lossy;
  lossy.loss_fraction = 0.05;
  netsim::ImpairedQueue<int> lossy_queue(lossy);
  for (int index = 0; index < 100000; ++index) {
    lossy_queue.offer(0.0, index);
    lossy_queue.take_due(0.0, item);
  }
  const double lost = double(lossy_queue.counters().dropped) / 100000.0;
  KCD2S_CHECK(near(lost, 0.05, 0.005));
  KCD2S_CHECK_EQ(lossy_queue.counters().delivered + lossy_queue.counters().dropped,
                 std::uint64_t{100000});

  netsim::ImpairmentSettings bounded;
  bounded.delay_seconds = 1.0;
  bounded.max_queued = 2;
  netsim::ImpairedQueue<int> small(bounded);
  small.offer(0.0, 1);
  small.offer(0.0, 2);
  small.offer(0.0, 3);
  KCD2S_CHECK_EQ(small.queued(), std::size_t{2});
  KCD2S_CHECK_EQ(small.counters().overflow, std::uint64_t{1});
  KCD2S_CHECK(!netsim::ImpairmentSettings{}.active());
}

KCD2S_TEST(load_metrics, a_spawn_walk_player_moves_continuously_around_its_spawn) {
  KCD2S_CHECK(
      load::parse_load_options({"--reference-player", "1", "--reference-layout", "spawn-walk"})
          .reference_layout == load::ReferenceLayout::SpawnWalk);
  const core::Vec3 spawn{100.0f, 200.0f, 10.0f};
  constexpr float kWalkSpeed = 1.4f;
  auto previous = load::walk_pose(spawn, 7, 1000, kWalkSpeed).position;
  for (std::int64_t clock = 1016; clock < 20000; clock += 16) {
    const auto pose = load::walk_pose(spawn, 7, clock, kWalkSpeed);
    // One 16 ms step at walking pace, on the wide circle round the spawn.
    KCD2S_CHECK(core::distance(previous, pose.position) < 0.1f);
    KCD2S_CHECK(core::distance(spawn, pose.position) < 7.5f);
    KCD2S_CHECK_EQ(pose.position.z, spawn.z);
    previous = pose.position;
  }
  // No speed: the player stands at its spawn.
  const auto still = load::walk_pose(spawn, 7, 5000, 0.0f);
  KCD2S_CHECK(core::distance(spawn, still.position) < 0.001f);
}

KCD2S_TEST(load_metrics, a_walk_clip_loop_keeps_raising_the_sequences) {
  local::AnimationState state{};
  state[0].clip = 16442;
  load::ReferencePose pose;
  pose.position = {1.0f, 2.0f, 3.0f};
  pose.fields.emplace_back(Velocity, core::Vec3{3.0f, 0.0f, 0.0f});
  pose.fields.emplace_back(Animations, std::string{});
  load::WalkClipLoop loop;
  for (std::int64_t clock = 1000; clock <= 1000 + load::WalkClipLoop::kRoundMs; clock += 500) {
    state[0].sequence = std::uint64_t(clock / 500);
    pose.owner_clock_ms = clock;
    pose.fields[1].second = local::encode_animation(state);
    loop.observe(pose);
  }
  KCD2S_CHECK(loop.complete());
  std::uint64_t previous = 0;
  for (std::int64_t clock = 0; clock < 30000; clock += 100) {
    const auto decoded = local::decode_animation(loop.clips(0, clock));
    KCD2S_CHECK(decoded.has_value());
    KCD2S_CHECK((*decoded)[0].sequence >= previous);
    previous = (*decoded)[0].sequence;
  }
  KCD2S_CHECK(previous > 40);
}
