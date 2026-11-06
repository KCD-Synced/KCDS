// kcd2s-load-probe: headless KSTP load generator for the measurement tooling.
//
// Connects N real UDP clients (the same Client used by kcd2s-network-probe) to a running
// kcd2s-server, acknowledges the server world like the network probe does, optionally drives
// synthetic movement, and prints machine-readable results:
//
//   KCDS_LOAD_PROGRESS {...}  periodically while running
//   KCDS_LOAD_CLIENT {...}    one line per client at the end
//   KCDS_LOAD_SUMMARY {...}   aggregate results, settings and the generator's own CPU use
//
// Resource lane: every client acknowledges SDK entity frames (directory/SharedState transfers) like
// the client resource runtime does, counts them, and measures the delivery latency of resource
// events named kcds-capacity-ping whose JSON payload carries {"t": <sender unix ms>}.
// --loss / --delay-ms emulate a lossy, delayed path on every client socket (netsim/).
//
// This is a measurement tool, not a game client and not capacity evidence by itself. All clients
// of one process run on one thread; the summary reports how busy that thread was so a saturated
// generator is not mistaken for a slow server. Several processes can share one layout
// (--index-offset, --layout-count); Dev/tools/load/run_load_baseline.py merges their results.
#include "../runtime.hpp"
#include "../test_passes.hpp"
#include "../animation.hpp"
#include "../rider_state.hpp"
#include "../metrics/counters.hpp"
#include "../metrics/duration_histogram.hpp"
#include "../metrics/json_object.hpp"
#include "../metrics/metrics_settings.hpp"
#include "../metrics/process_usage_win.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "load_options.hpp"
#include "markers.hpp"
#include "reference_pose.hpp"
#include "workload.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace kcd2s;
using namespace kcd2s::local;
using metrics::DurationHistogram;
using metrics::JsonObject;
using metrics::TrafficCounters;

namespace {

enum class Phase { Joining, Warmup, Measuring, Done };

const char* phase_name(Phase phase) {
  switch (phase) {
    case Phase::Joining:
      return "joining";
    case Phase::Warmup:
      return "warmup";
    case Phase::Measuring:
      return "measuring";
    case Phase::Done:
      return "done";
  }
  return "unknown";
}

struct ProbeClient {
  std::unique_ptr<Client> client;
  load::CirclePath path;
  double established_at = -1.0;  // seconds since probe start
  double world_ready_at = -1.0;
  double avatar_at = -1.0;
  core::EntityId avatar = core::kInvalidEntity;

  // Sender side: every movement input carries a marker (see markers.hpp) in the owner-authored
  // motionMask field.
  std::int64_t last_marker = 0;
  std::uint64_t reference_poses_forwarded = 0;
  std::uint64_t reference_transforms_adopted = 0;
  load::ReferenceSpawnPosition reference_spawn;
  load::ReferenceFollowPosition reference_follow;
  std::optional<std::uint32_t> reference_revision;

  // Receiver side.
  core::Tick last_baseline = 0;
  std::uint64_t applied_snapshots = 0;  // baseline advances during the measurement window
  std::uint64_t remote_updates = 0;     // newer remote markers seen during the window
  std::unordered_map<core::EntityId, std::int64_t> last_seen_marker;
  DurationHistogram convergence;
  TrafficCounters window_start;
  TrafficCounters window_end;
  std::size_t visible_players = 0;

  // Resource lane during the measurement window.
  std::uint64_t sdk_frames = 0;  // entitySession/Begin/Chunk/Commit received and acknowledged
  std::uint64_t sdk_acks_refused = 0;
  std::uint64_t events = 0;  // resource events received
  DurationHistogram event_latency;
};

bool is_entity_frame(const std::string& op) {
  return op == "entitySession" || op == "entityBegin" || op == "entityChunk" ||
         op == "entityCommit";
}

// Sender timestamp of a kcds-capacity-ping payload ({"t": <unix ms>, ...}), or -1.
std::int64_t ping_timestamp(const sdk::Value& message) {
  const auto* name = integration::script_field(message, "name");
  const auto* payload = integration::script_field(message, "payload");
  if (!name || !payload || name->tag() != sdk::Tag::Str || payload->tag() != sdk::Tag::Str) {
    return -1;
  }
  if (name->as_str() != "kcds-capacity-ping") {
    return -1;
  }
  const std::string& text = payload->as_str();
  const auto key = text.find("\"t\":");
  if (key == std::string::npos) {
    return -1;
  }
  try {
    return std::stoll(text.substr(key + 4));
  } catch (const std::exception&) {
    return -1;
  }
}

// Acknowledges SDK entity frames and records resource events of one client.
void drain_scripts(ProbeClient& probe, bool measuring) {
  for (const sdk::Value& message : probe.client->peer().take_scripts()) {
    const std::string op = integration::script_operation(message);
    if (is_entity_frame(op)) {
      const auto* session = integration::script_field(message, "session");
      const auto* delivery = integration::script_field(message, "delivery");
      if (session && delivery && session->tag() == sdk::Tag::Str) {
        sdk::Value::Map ack{
            {"op", sdk::Value("entityAck")}, {"session", *session}, {"delivery", *delivery}};
        if (!probe.client->peer().send_script(sdk::Value(std::move(ack))) && measuring) {
          probe.sdk_acks_refused += 1;
        }
      }
      if (measuring) {
        probe.sdk_frames += 1;
      }
      continue;
    }
    if (op == "event") {
      if (!measuring) {
        continue;
      }
      probe.events += 1;
      const std::int64_t sent_ms = ping_timestamp(message);
      if (sent_ms > 0) {
        const std::int64_t age = metrics::unix_milliseconds() - sent_ms;
        probe.event_latency.add(age < 0 ? 0.0 : double(age));
      }
    }
  }
}

JsonObject impairment_object(const netsim::ImpairmentCounters& counters) {
  JsonObject object;
  object.add_count("offered", counters.offered);
  object.add_count("dropped", counters.dropped);
  object.add_count("overflow", counters.overflow);
  object.add_count("delivered", counters.delivered);
  return object;
}

std::vector<std::pair<core::FieldId, replication::FieldValue>> build_input(
    const load::LoadOptions& options, const load::MovementSample& movement, std::int64_t marker,
    const std::string& animations, const std::string& rider) {
  std::vector<std::pair<core::FieldId, replication::FieldValue>> input{
      {Position, movement.position},
      {Rotation, movement.rotation},
      {Velocity, movement.velocity},
      {MotionMask, marker}};
  if (options.input_fields == load::InputFields::Compact) {
    return input;
  }
  // Same owner fields as the game client input (Client/runtime), with synthetic values.
  input.emplace_back(PoseValid, true);
  input.emplace_back(LookTuning, core::Vec3{});
  input.emplace_back(LookTarget, movement.position);
  input.emplace_back(LookEnabled, false);
  input.emplace_back(LookValid, false);
  input.emplace_back(NativeRiderState, rider);
  input.emplace_back(Animations, animations);
  input.emplace_back(MotionValid, true);
  input.emplace_back(MotionType, std::int64_t{1});
  input.emplace_back(MotionVelocity, movement.velocity);
  input.emplace_back(MotionRotation, movement.rotation);
  input.emplace_back(MotionFlags, std::int64_t{1});
  for (unsigned i = 0; i < 6; ++i) {
    input.emplace_back(core::FieldId(MotionParams0 + i), core::Vec3{});
  }
  return input;
}

std::string running_animation() {
  AnimationState state{};
  auto& layer = state[0];
  layer.sequence = 1;
  layer.clip = 101;
  layer.params.flags = 0x102;
  return encode_animation(state);
}

// Scan a receiver's mirror after it applied a new snapshot: for every remote player whose marker
// advanced, the age of that marker is one convergence sample. Senders may be in other processes.
void observe_convergence(ProbeClient& receiver, std::int64_t received_at_us, bool measuring,
                         const std::unordered_set<core::EntityId>* reference_peers = nullptr) {
  const auto player_type = core::type_id("NetworkPlayer");
  std::size_t visible = 0;
  for (const auto& [entity, state] : receiver.client->peer().mirror().entities()) {
    if (state.type != player_type || entity == receiver.avatar) {
      continue;
    }
    visible += 1;
    if (reference_peers && !reference_peers->count(entity)) {
      continue;
    }
    const auto marker_field = reference_peers ? MotionClock : MotionMask;
    if (marker_field >= state.fields.size()) {
      continue;
    }
    const auto* value = std::get_if<std::int64_t>(&state.fields[marker_field]);
    const std::int64_t marker =
        value ? (reference_peers ? load::reference_marker_microseconds(*value) : *value) : 0;
    if (marker <= 0) {
      continue;
    }
    auto& last = receiver.last_seen_marker[entity];
    if (marker <= last) {
      continue;
    }
    // The first marker seen for an entity may be old (it arrived in a CONSTRUCT); only count
    // advances of a marker that was already known.
    const bool advanced = last != 0;
    last = marker;
    if (measuring && advanced) {
      receiver.remote_updates += 1;
      receiver.convergence.add(load::marker_age_milliseconds(marker, received_at_us));
    }
  }
  receiver.visible_players = visible;
}

JsonObject traffic_object(const TrafficCounters& traffic, double seconds) {
  JsonObject object;
  object.add_count("packets_sent", traffic.packets_sent);
  object.add_count("bytes_sent", traffic.bytes_sent);
  object.add_count("packets_received", traffic.packets_received);
  object.add_count("bytes_received", traffic.bytes_received);
  object.add_count("send_would_block", traffic.send_would_block);
  if (seconds > 0.0) {
    object.add_number("bytes_sent_per_s", double(traffic.bytes_sent) / seconds);
    object.add_number("bytes_received_per_s", double(traffic.bytes_received) / seconds);
    object.add_number("packets_received_per_s", double(traffic.packets_received) / seconds);
  }
  return object;
}

JsonObject settings_object(const load::LoadOptions& options) {
  JsonObject settings;
  settings.add_text("host", options.host);
  settings.add_count("port", options.port);
  settings.add_count("clients", options.clients);
  settings.add_text("workload", load::workload_name(options.workload));
  settings.add_text("input_fields",
                    options.input_fields == load::InputFields::Full ? "full" : "compact");
  settings.add_number("duration_s", options.duration_seconds);
  settings.add_number("join_timeout_s", options.join_timeout_seconds);
  settings.add_number("warmup_s", options.warmup_seconds);
  settings.add_number("tick_hz", options.tick_hz);
  settings.add_number("input_hz", options.input_hz);
  settings.add_number("spacing_m", options.spacing_meters);
  settings.add_number("dense_radius_m", options.dense_radius_meters);
  settings.add_number("move_radius_m", options.move_radius_meters);
  settings.add_number("speed_mps", options.speed_meters_per_second);
  settings.add_count("index_offset", options.index_offset);
  settings.add_count("layout_count", options.total_players());
  settings.add_count("reference_player", options.reference_player);
  settings.add_text("reference_layout", load::reference_layout_name(options.reference_layout));
  settings.add_text("reference_height",
                    options.reference_layout == load::ReferenceLayout::SpawnFollow
                        ? "source-z-delta; translated terrain unvalidated"
                        : "unchanged layout behavior");
  settings.add_text("reference_updates", load::reference_updates_name(options.reference_updates));
  settings.add_text("reference_capture", options.reference_capture);
  settings.add_text("native_scene", !options.reference_player ? "disabled"
                                    : options.reference_layout == load::ReferenceLayout::SpawnFollow
                                        ? "reference_spawn_follow"
                                    : options.reference_layout == load::ReferenceLayout::Spawn
                                        ? "reference_spawn"
                                        : "reference_ring");
  settings.add_number("loss_fraction", options.loss_fraction);
  settings.add_number("delay_ms_one_way", options.delay_ms);
  settings.add_text("leave", options.leave == load::LeaveMode::Graceful ? "graceful" : "timeout");
  return settings;
}

JsonObject reference_diagnostics_object(const load::ReferenceDiagnostics& diagnostics) {
  JsonObject result;
  result.add_count("valid_owner_clock_observations", diagnostics.observations());
  result.add_count("idle_observations", diagnostics.idle_observations());
  const auto* first = diagnostics.first();
  if (first) {
    JsonObject initial;
    initial.add_flag("idle", diagnostics.first_idle());
    initial.add_integer("MotionClock", std::get<std::int64_t>(first->fields[MotionClock]));
    for (const auto [field, name] : {std::pair{MotionFlags, "MotionFlags"},
                                     {Posture, "Posture"},
                                     {WeaponState, "WeaponState"},
                                     {HeldProp, "HeldProp"}}) {
      initial.add_integer(name, std::get<std::int64_t>(first->fields[field]));
    }
    initial.add_flag("AimEnabled", std::get<bool>(first->fields[AimEnabled]));
    initial.add_flag("PoseValid", std::get<bool>(first->fields[PoseValid]));
    initial.add_flag("MotionValid", std::get<bool>(first->fields[MotionValid]));
    initial.add_flag("BodyVisible", std::get<bool>(first->fields[BodyVisible]));
    for (const auto [field, name] :
         {std::pair{Velocity, "Velocity"}, {MotionVelocity, "MotionVelocity"}}) {
      const auto value = std::get<core::Vec3>(first->fields[field]);
      JsonObject vector;
      vector.add_number("x", value.x);
      vector.add_number("y", value.y);
      vector.add_number("z", value.z);
      initial.add_object(name, vector);
    }
    for (const auto [field, name] : {std::pair{NativeRiderState, "NativeRiderState_bytes"},
                                     {RagdollPose, "RagdollPose_bytes"},
                                     {Seats, "Seats_bytes"},
                                     {Carry, "Carry_bytes"},
                                     {StationAction, "StationAction_bytes"},
                                     {AnimationProps, "AnimationProps_bytes"},
                                     {Animations, "Animations_bytes"},
                                     {WeaponAnimations, "WeaponAnimations_bytes"}}) {
      initial.add_count(name, std::get<std::string>(first->fields[field]).size());
    }
    result.add_object("first_valid", initial);
  }
  static const auto schema = player_schema();
  const auto* type = schema.get(core::type_id("NetworkPlayer"));
  JsonObject changes;
  for (std::size_t i = 0; i < diagnostics.changes().size(); ++i) {
    if (diagnostics.changes()[i]) {
      changes.add_count(type->fields[i].name, diagnostics.changes()[i]);
    }
  }
  result.add_object("semantic_changed_fields", changes);
  result.add_text("change_definition",
                  "successive valid owner-clock observations; excludes "
                  "MotionClock and same-start animation normalized_time");
  return result;
}

int run(const load::LoadOptions& options) {
  Winsock winsock;
  const auto port = static_cast<unsigned short>(options.port);
  const std::string animations = options.reference_player ? std::string{} : running_animation();
  const std::string rider = encode_rider(RiderState{});
  const double tick_period = 1.0 / options.tick_hz;
  const double input_period = 1.0 / options.input_hz;
  const bool moving = options.workload != load::Workload::Idle || options.reference_player != 0;

  const double start = clock_seconds();
  const auto start_usage = metrics::sample_process_usage();
  std::vector<ProbeClient> clients(options.clients);
  std::unordered_set<core::EntityId> probe_avatars;
  load::ReferenceSource reference_source(options.reference_updates);
  load::WalkClipLoop walk_clips;
  load::ReferenceAnimationCapture reference_capture(options.reference_capture);
  load::ReferenceDiagnostics reference_diagnostics;
  std::uint32_t reference_world_revision = 0;
  // A join pass for each client when the harness runs a test Master.
  const auto passes = test_passes(port);
  for (std::size_t i = 0; i < clients.size(); ++i) {
    clients[i].client = std::make_unique<Client>(options.host.c_str(), port);
    if (passes) {
      clients[i].client->set_licence_provider(passes);
    }
    netsim::ImpairmentSettings impairment;
    impairment.loss_fraction = options.loss_fraction;
    impairment.delay_seconds = options.delay_ms / 1000.0;
    impairment.seed = 0x9e3779b97f4a7c15ull + options.index_offset + i;
    clients[i].client->set_impairment(impairment);
    clients[i].path = load::circle_for(options, options.index_offset + i, options.total_players());
  }

  Phase phase = Phase::Joining;
  double phase_started = start;
  double window_start = 0.0;
  double window_end = 0.0;
  std::int64_t window_start_unix_ms = 0;
  std::int64_t window_end_unix_ms = 0;
  metrics::ProcessUsage window_start_usage;
  metrics::ProcessUsage window_end_usage;
  double window_busy_seconds = 0.0;
  std::uint64_t window_ticks = 0;
  std::uint64_t window_late_ticks = 0;
  std::uint64_t window_inputs = 0;
  std::uint64_t window_skipped_input_slots = 0;
  std::uint64_t reference_missing_inputs = 0;
  std::uint64_t reference_invalid_inputs = 0;
  std::uint64_t reference_missing_spawn_inputs = 0;
  std::uint64_t reference_follow_waiting_inputs = 0;
  std::uint64_t reference_forwarded_inputs = 0;
  std::uint64_t reference_window_inputs = 0;
  std::uint64_t reference_frozen_captures = 0;
  std::uint64_t reference_waiting_idle_inputs = 0;
  std::int64_t reference_frozen_clock_ms = 0;
  DurationHistogram window_tick_work;
  double next_tick = start;
  double next_input = start;
  double next_progress = start + options.report_interval_seconds;

  while (phase != Phase::Done) {
    const double tick_start = clock_seconds();
    const bool measuring = phase == Phase::Measuring;
    std::size_t admitted = 0;
    std::size_t ready = 0;

    for (std::size_t i = 0; i < clients.size(); ++i) {
      ProbeClient& probe = clients[i];
      probe.client->tick(tick_start);
      if (probe.established_at < 0.0 && probe.client->established()) {
        probe.established_at = clock_seconds() - start;
      }
      auto& peer = probe.client->peer();
      if (options.reference_player && peer.world_config()) {
        const auto revision = peer.world_config()->revision;
        reference_world_revision = std::max(reference_world_revision, revision);
        if (!probe.reference_revision || *probe.reference_revision != revision) {
          probe.reference_revision = revision;
          probe_avatars.erase(probe.avatar);
          probe.avatar = core::kInvalidEntity;
          probe.reference_spawn = {};
          probe.reference_follow = {};
        }
      }
      if (options.reference_layout == load::ReferenceLayout::SpawnFollow &&
          probe.avatar != core::kInvalidEntity &&
          (!probe.client->established() || !peer.owns(probe.avatar))) {
        probe_avatars.erase(probe.avatar);
        probe.avatar = core::kInvalidEntity;
        probe.reference_spawn = {};
        probe.reference_follow = {};
      }
      if (peer.world_config() && !peer.world_ready()) {
        // Headless diagnostic only: emulate the engine's world-loaded callback.
        if (peer.mark_world_ready(peer.world_config()->revision) && probe.world_ready_at < 0.0) {
          probe.world_ready_at = clock_seconds() - start;
        }
      }
      if (probe.avatar == core::kInvalidEntity && peer.world_ready() &&
          (options.reference_layout != load::ReferenceLayout::SpawnFollow ||
           probe.client->established())) {
        probe.avatar = probe.client->own_avatar();
        if (probe.avatar != core::kInvalidEntity) {
          probe.avatar_at = clock_seconds() - start;
          probe_avatars.insert(probe.avatar);
        }
      }
      if (probe.client->established()) {
        admitted += 1;
        drain_scripts(probe, measuring);
      }
      if (probe.avatar != core::kInvalidEntity) {
        ready += 1;
        if (options.reference_player) {
          probe.reference_transforms_adopted += load::adopt_reference_transforms(
              peer, probe.avatar, probe.reference_spawn,
              options.reference_layout == load::ReferenceLayout::SpawnFollow
                  ? &probe.reference_follow
                  : nullptr);
        }
        if (options.reference_player && options.reference_layout != load::ReferenceLayout::Ring) {
          probe.reference_spawn.capture(peer.mirror().get(probe.avatar));
        }
      }
      const core::Tick baseline = peer.baseline();
      if (baseline != probe.last_baseline) {
        probe.last_baseline = baseline;
        if (measuring) {
          probe.applied_snapshots += 1;
        }
        observe_convergence(probe, load::marker_clock_microseconds(), measuring,
                            options.reference_player ? &probe_avatars : nullptr);
      }
    }

    if (moving && tick_start >= next_input) {
      const double elapsed = tick_start - start;
      const load::ReferencePose* reference = nullptr;
      bool reference_seen = probe_avatars.count(options.reference_player) != 0;
      if (options.reference_player && !reference_seen) {
        const replication::Mirror::Entity* newest = nullptr;
        std::int64_t newest_clock = -1;
        for (const auto& probe : clients) {
          if (!probe.reference_revision || *probe.reference_revision != reference_world_revision) {
            continue;
          }
          const auto* state = probe.client->peer().mirror().get(options.reference_player);
          if (!state) {
            continue;
          }
          reference_seen = true;
          if (reference_capture.observe(state)) {
            JsonObject captured;
            captured.add_text("path", options.reference_capture);
            captured.add_count("bytes", reference_capture.bytes());
            captured.add_count("reference_player", options.reference_player);
            captured.add_count("world_revision", reference_world_revision);
            std::cout << "KCDS_LOAD_REFERENCE_CAPTURE " << captured.str() << std::endl;
          }
          const auto* clock = state->fields.size() > MotionClock
                                  ? std::get_if<std::int64_t>(&state->fields[MotionClock])
                                  : nullptr;
          if (!newest || (clock && *clock > newest_clock)) {
            newest = state;
            newest_clock = clock ? *clock : -1;
          }
        }
        if (reference_diagnostics.observe(newest, reference_world_revision)) {
          std::cout << "KCDS_LOAD_REFERENCE_DIAGNOSTIC "
                    << reference_diagnostics_object(reference_diagnostics).str() << std::endl;
        }
        const bool was_frozen = reference_source.frozen();
        reference = reference_source.observe(newest, reference_world_revision, tick_start);
        if (!was_frozen && reference_source.frozen()) {
          ++reference_frozen_captures;
          reference_frozen_clock_ms = reference->owner_clock_ms;
          JsonObject frozen;
          frozen.add_text("updates", "frozen-idle");
          frozen.add_count("reference_player", options.reference_player);
          frozen.add_count("world_revision", reference_world_revision);
          frozen.add_integer("source_motion_clock_ms", reference_frozen_clock_ms);
          frozen.add_number("elapsed_s", elapsed);
          std::cout << "KCDS_LOAD_REFERENCE_FROZEN " << frozen.str() << std::endl;
        }
        reference_seen = reference_seen || reference_source.frozen();
      }
      const bool walks = options.reference_layout == load::ReferenceLayout::SpawnWalk;
      std::optional<core::Vec3> keep_off;
      if (walks && reference) {
        keep_off = reference->position;
        walk_clips.observe(*reference);
        if (walk_clips.complete()) {
          reference = &walk_clips.pose();
        }
      }
      for (std::size_t index = 0; index < clients.size(); ++index) {
        auto& probe = clients[index];
        if (probe.avatar == core::kInvalidEntity) {
          continue;
        }
        if (options.reference_player) {
          if (!reference || !probe.reference_revision ||
              *probe.reference_revision != reference_world_revision) {
            probe.reference_follow.source_missing();
            if (options.reference_updates == load::ReferenceUpdates::FrozenIdle && !reference) {
              ++reference_waiting_idle_inputs;
            }
            if (reference_seen) {
              ++reference_invalid_inputs;
            } else {
              ++reference_missing_inputs;
            }
            continue;
          }
          if (options.reference_layout != load::ReferenceLayout::Ring &&
              !probe.reference_spawn.get()) {
            ++reference_missing_spawn_inputs;
            continue;
          }
          std::optional<core::Vec3> followed;
          if (options.reference_layout == load::ReferenceLayout::SpawnFollow) {
            auto& peer = probe.client->peer();
            followed = probe.reference_follow.observe(
                *reference, probe.avatar, reference_world_revision, peer.combat_epoch(probe.avatar),
                peer.mirror().get(probe.avatar), probe.reference_spawn.get());
            if (!followed) {
              ++reference_follow_waiting_inputs;
              continue;
            }
          }
          const auto clock_ms =
              load::next_marker(probe.last_marker / 1000, load::marker_clock_microseconds() / 1000);
          probe.last_marker = load::reference_marker_microseconds(clock_ms);
          auto input =
              load::reference_input(*reference, options, options.index_offset + index, clock_ms,
                                    probe.reference_spawn.get(), followed, keep_off);
          if (input.empty()) {
            ++reference_invalid_inputs;
            continue;
          }
          if (walks && walk_clips.complete()) {
            for (auto& [field, value] : input) {
              if (field == Animations) {
                value = walk_clips.clips(options.index_offset + index, clock_ms);
              }
            }
          }
          probe.client->peer().send_input(probe.avatar, input);
          ++probe.reference_poses_forwarded;
          ++reference_forwarded_inputs;
          if (measuring) {
            ++reference_window_inputs;
          }
        } else {
          const auto movement =
              load::sample_movement(probe.path, options.speed_meters_per_second, elapsed);
          const std::int64_t marker =
              load::next_marker(probe.last_marker, load::marker_clock_microseconds());
          probe.last_marker = marker;
          probe.client->peer().send_input(
              probe.avatar, build_input(options, movement, marker, animations, rider));
        }
        if (measuring) {
          window_inputs += 1;
        }
      }
      next_input += input_period;
      if (next_input < tick_start) {
        // The generator fell behind its input schedule; do not burst to catch up.
        if (measuring) {
          window_skipped_input_slots +=
              static_cast<std::uint64_t>((tick_start - next_input) / input_period);
        }
        next_input = tick_start + input_period;
      }
    }

    const double tick_end = clock_seconds();
    if (measuring) {
      window_busy_seconds += tick_end - tick_start;
      window_tick_work.add((tick_end - tick_start) * 1000.0);
      window_ticks += 1;
    }

    // Phase transitions.
    const double now = tick_end;
    if (phase == Phase::Joining &&
        (ready == clients.size() || now - start >= options.join_timeout_seconds)) {
      phase = Phase::Warmup;
      phase_started = now;
    } else if (phase == Phase::Warmup && now - phase_started >= options.warmup_seconds) {
      phase = Phase::Measuring;
      phase_started = now;
      window_start = now;
      window_start_unix_ms = metrics::unix_milliseconds();
      window_start_usage = metrics::sample_process_usage();
      for (auto& probe : clients) {
        probe.window_start = probe.client->traffic();
      }
    } else if (phase == Phase::Measuring && now - phase_started >= options.duration_seconds) {
      phase = Phase::Done;
      window_end = now;
      window_end_unix_ms = metrics::unix_milliseconds();
      window_end_usage = metrics::sample_process_usage();
      for (auto& probe : clients) {
        probe.window_end = probe.client->traffic();
      }
    }

    if (now >= next_progress) {
      next_progress += options.report_interval_seconds;
      JsonObject progress;
      progress.add_text("schema", "kcds.load.progress.v1");
      progress.add_integer("unix_ms", metrics::unix_milliseconds());
      progress.add_number("elapsed_s", now - start);
      progress.add_text("phase", phase_name(phase));
      progress.add_count("admitted", admitted);
      progress.add_count("avatars_ready", ready);
      progress.add_count("requested", clients.size());
      std::cout << "KCDS_LOAD_PROGRESS " << progress.str() << std::endl;
    }

    next_tick += tick_period;
    const double after = clock_seconds();
    if (next_tick < after) {
      if (measuring) {
        window_late_ticks += 1;
      }
      next_tick = after;
    } else {
      std::this_thread::sleep_for(std::chrono::duration<double>(next_tick - after));
    }
  }

  if (options.leave == load::LeaveMode::Graceful) {
    for (auto& probe : clients) {
      probe.client->disconnect();
    }
  }

  const double window_seconds = window_end - window_start;
  DurationHistogram convergence_all;
  DurationHistogram event_latency_all;
  std::uint64_t sdk_frames_total = 0;
  std::uint64_t sdk_acks_refused_total = 0;
  std::uint64_t events_total = 0;
  netsim::ImpairmentCounters impairment_up;
  netsim::ImpairmentCounters impairment_down;
  DurationHistogram join_times;
  double snapshot_hz_min = 0.0;
  double snapshot_hz_max = 0.0;
  double snapshot_hz_sum = 0.0;
  TrafficCounters admitted_traffic;
  TrafficCounters refused_traffic;
  std::size_t admitted_count = 0;
  std::size_t refused_clients = 0;
  std::uint64_t refusals_received = 0;
  std::size_t ready_count = 0;
  std::size_t visible_min = 0;
  std::size_t visible_max = 0;
  std::size_t visible_sum = 0;
  double update_rate_sum = 0.0;  // remote updates per second per visible player, summed

  for (std::size_t i = 0; i < clients.size(); ++i) {
    ProbeClient& probe = clients[i];
    const TrafficCounters window = metrics::traffic_delta(probe.window_end, probe.window_start);
    const bool admitted = probe.established_at >= 0.0;
    const std::uint64_t refusals = probe.client->refusals_received();
    refusals_received += refusals;
    if (refusals > 0) {
      refused_clients += 1;
    }
    const double snapshot_hz =
        window_seconds > 0.0 ? double(probe.applied_snapshots) / window_seconds : 0.0;
    const double remote_updates_per_s =
        window_seconds > 0.0 ? double(probe.remote_updates) / window_seconds : 0.0;
    const double update_rate_per_visible =
        probe.visible_players ? remote_updates_per_s / double(probe.visible_players) : 0.0;
    if (admitted) {
      admitted_count += 1;
      join_times.add(probe.established_at * 1000.0);
      admitted_traffic.packets_sent += window.packets_sent;
      admitted_traffic.bytes_sent += window.bytes_sent;
      admitted_traffic.packets_received += window.packets_received;
      admitted_traffic.bytes_received += window.bytes_received;
      admitted_traffic.send_would_block += window.send_would_block;
    } else {
      refused_traffic.packets_sent += window.packets_sent;
      refused_traffic.bytes_sent += window.bytes_sent;
      refused_traffic.packets_received += window.packets_received;
      refused_traffic.bytes_received += window.bytes_received;
      refused_traffic.send_would_block += window.send_would_block;
    }
    if (probe.avatar != core::kInvalidEntity) {
      if (ready_count == 0 || snapshot_hz < snapshot_hz_min) {
        snapshot_hz_min = snapshot_hz;
      }
      if (snapshot_hz > snapshot_hz_max) {
        snapshot_hz_max = snapshot_hz;
      }
      snapshot_hz_sum += snapshot_hz;
      if (ready_count == 0 || probe.visible_players < visible_min) {
        visible_min = probe.visible_players;
      }
      if (probe.visible_players > visible_max) {
        visible_max = probe.visible_players;
      }
      visible_sum += probe.visible_players;
      update_rate_sum += update_rate_per_visible;
      ready_count += 1;
    }
    convergence_all.merge(probe.convergence);
    event_latency_all.merge(probe.event_latency);
    sdk_frames_total += probe.sdk_frames;
    sdk_acks_refused_total += probe.sdk_acks_refused;
    events_total += probe.events;
    const auto up = probe.client->socket().outbound_impairment();
    const auto down = probe.client->socket().inbound_impairment();
    impairment_up.offered += up.offered;
    impairment_up.dropped += up.dropped;
    impairment_up.overflow += up.overflow;
    impairment_up.delivered += up.delivered;
    impairment_down.offered += down.offered;
    impairment_down.dropped += down.dropped;
    impairment_down.overflow += down.overflow;
    impairment_down.delivered += down.delivered;

    JsonObject line;
    line.add_text("schema", "kcds.load.client.v1");
    line.add_count("index", i);
    line.add_flag("admitted", admitted);
    line.add_count("refusals_received", refusals);
    line.add_number("join_ms", admitted ? probe.established_at * 1000.0 : -1.0);
    line.add_number("world_ready_ms",
                    probe.world_ready_at >= 0.0 ? probe.world_ready_at * 1000.0 : -1.0);
    line.add_number("avatar_ms", probe.avatar_at >= 0.0 ? probe.avatar_at * 1000.0 : -1.0);
    line.add_count("avatar", probe.avatar);
    line.add_count("reference_poses_forwarded_whole_run", probe.reference_poses_forwarded);
    line.add_count("reference_transforms_adopted", probe.reference_transforms_adopted);
    if (const auto& spawn = probe.reference_spawn.get()) {
      line.add_number("reference_spawn_x", spawn->x);
      line.add_number("reference_spawn_y", spawn->y);
      line.add_number("reference_spawn_z", spawn->z);
    }
    line.add_count("visible_players", probe.visible_players);
    line.add_number("applied_snapshots_per_s", snapshot_hz);
    line.add_number("remote_updates_per_s", remote_updates_per_s);
    line.add_number("updates_per_s_per_visible_player", update_rate_per_visible);
    line.add_object("window_traffic", traffic_object(window, window_seconds));
    line.add_object("convergence", metrics::describe(probe.convergence));
    line.add_count("sdk_frames", probe.sdk_frames);
    line.add_count("events", probe.events);
    std::cout << "KCDS_LOAD_CLIENT " << line.str() << std::endl;
  }

  const double cpu_window = metrics::cpu_percent_of_one_core(window_start_usage, window_end_usage);
  const unsigned cores = metrics::logical_core_count();

  JsonObject joins;
  joins.add_count("requested", clients.size());
  joins.add_count("admitted", admitted_count);
  joins.add_count("not_admitted", clients.size() - admitted_count);
  joins.add_count("avatars_ready", ready_count);
  joins.add_count("clients_refused_server_full", refused_clients);
  joins.add_count("refusals_received_total", refusals_received);
  joins.add_object("time_to_join", metrics::describe(join_times));

  JsonObject traffic;
  traffic.add_object("admitted_clients", traffic_object(admitted_traffic, window_seconds));
  traffic.add_object("not_admitted_clients", traffic_object(refused_traffic, window_seconds));
  if (admitted_count > 0 && window_seconds > 0.0) {
    traffic.add_number(
        "bytes_sent_per_admitted_client_per_s",
        double(admitted_traffic.bytes_sent) / double(admitted_count) / window_seconds);
    traffic.add_number(
        "bytes_received_per_admitted_client_per_s",
        double(admitted_traffic.bytes_received) / double(admitted_count) / window_seconds);
  }

  JsonObject replication;
  JsonObject rates;
  rates.add_count("clients", ready_count);
  rates.add_number("min_hz", snapshot_hz_min);
  rates.add_number("mean_hz", ready_count ? snapshot_hz_sum / double(ready_count) : 0.0);
  rates.add_number("max_hz", snapshot_hz_max);
  replication.add_object("applied_snapshots_per_client", rates);
  replication.add_count("visible_players_min", visible_min);
  replication.add_number("visible_players_mean",
                         ready_count ? double(visible_sum) / double(ready_count) : 0.0);
  replication.add_count("visible_players_max", visible_max);
  replication.add_number("updates_per_s_per_visible_player_mean",
                         ready_count ? update_rate_sum / double(ready_count) : 0.0);
  replication.add_object("convergence", metrics::describe(convergence_all));
  replication.add_object("convergence_histogram", metrics::histogram_buckets(convergence_all));
  replication.add_text("convergence_definition",
                       "time from queueing an owner input (send_input) to another client "
                       "applying a snapshot that contains it, from the shared steady clock; "
                       "includes up to one generator tick of sender batching and receiver "
                       "polling");

  JsonObject sdk;
  sdk.add_count("entity_frames", sdk_frames_total);
  sdk.add_count("entity_acks_refused", sdk_acks_refused_total);
  sdk.add_number("entity_frames_per_client_per_s",
                 ready_count && window_seconds > 0.0
                     ? double(sdk_frames_total) / double(ready_count) / window_seconds
                     : 0.0);
  sdk.add_count("events", events_total);
  sdk.add_number("events_per_client_per_s",
                 ready_count && window_seconds > 0.0
                     ? double(events_total) / double(ready_count) / window_seconds
                     : 0.0);
  sdk.add_object("event_latency", metrics::describe(event_latency_all));
  sdk.add_object("event_latency_histogram", metrics::histogram_buckets(event_latency_all));
  sdk.add_text("event_latency_definition",
               "unix ms at the probe minus the unix ms the resource wrote into a "
               "kcds-capacity-ping payload; millisecond resolution, one machine clock");

  JsonObject impairment;
  impairment.add_object("client_upload", impairment_object(impairment_up));
  impairment.add_object("client_download", impairment_object(impairment_down));

  JsonObject generator;
  generator.add_number("cpu_percent_of_one_core", cpu_window);
  generator.add_count("logical_cores", cores);
  generator.add_number("cpu_percent_of_machine", cores ? cpu_window / double(cores) : 0.0);
  generator.add_number("loop_busy_fraction",
                       window_seconds > 0.0 ? window_busy_seconds / window_seconds : 0.0);
  generator.add_count("loop_ticks", window_ticks);
  generator.add_number("achieved_tick_hz",
                       window_seconds > 0.0 ? double(window_ticks) / window_seconds : 0.0);
  generator.add_count("late_ticks", window_late_ticks);
  generator.add_object("tick_work", metrics::describe(window_tick_work));
  generator.add_number("busy_seconds", window_busy_seconds);
  generator.add_count("inputs_sent", window_inputs);
  generator.add_count("skipped_input_slots", window_skipped_input_slots);
  generator.add_count("working_set_bytes", window_end_usage.working_set_bytes);
  generator.add_count("peak_working_set_bytes", window_end_usage.peak_working_set_bytes);
  generator.add_number("cpu_percent_of_one_core_whole_run",
                       metrics::cpu_percent_of_one_core(start_usage, window_end_usage));
  generator.add_flag("single_threaded", true);

  JsonObject live_scene;
  live_scene.add_flag("enabled", options.reference_player != 0);
  live_scene.add_count("reference_player", options.reference_player);
  live_scene.add_text("reference_updates", load::reference_updates_name(options.reference_updates));
  live_scene.add_count("frozen_idle_captures", reference_frozen_captures);
  live_scene.add_integer("frozen_source_motion_clock_ms", reference_frozen_clock_ms);
  live_scene.add_count("waiting_frozen_idle_input_slots", reference_waiting_idle_inputs);
  live_scene.add_flag("reference_capture_requested", !options.reference_capture.empty());
  live_scene.add_flag("reference_capture_completed", reference_capture.completed());
  live_scene.add_count("reference_capture_bytes", reference_capture.bytes());
  live_scene.add_object("reference_diagnostics",
                        reference_diagnostics_object(reference_diagnostics));
  live_scene.add_count("forwarded_pose_inputs_whole_run", reference_forwarded_inputs);
  live_scene.add_count("forwarded_pose_inputs_window", reference_window_inputs);
  live_scene.add_count("missing_reference_input_slots", reference_missing_inputs);
  live_scene.add_count("invalid_reference_input_slots", reference_invalid_inputs);
  live_scene.add_count("missing_spawn_input_slots", reference_missing_spawn_inputs);
  live_scene.add_count("waiting_follow_input_slots", reference_follow_waiting_inputs);
  live_scene.add_text("follow_height",
                      options.reference_layout == load::ReferenceLayout::SpawnFollow
                          ? "source-z-delta; translated terrain unvalidated"
                          : "not applicable");
  live_scene.add_count("clients_with_forwarded_pose",
                       std::count_if(clients.begin(), clients.end(), [](const ProbeClient& probe) {
                         return probe.reference_poses_forwarded != 0;
                       }));
  live_scene.add_text("convergence_clock",
                      "normal owner MotionClock in milliseconds; local probe peers only");
  live_scene.add_text(
      "acceptance",
      "synthetic replicas; loaded models and animation require real-game observation");

  JsonObject summary;
  summary.add_text("schema", "kcds.load.summary.v1");
  summary.add_object("settings", settings_object(options));
  summary.add_integer("window_start_unix_ms", window_start_unix_ms);
  summary.add_integer("window_end_unix_ms", window_end_unix_ms);
  summary.add_number("window_s", window_seconds);
  summary.add_object("joins", joins);
  summary.add_object("traffic", traffic);
  summary.add_object("replication", replication);
  summary.add_object("sdk", sdk);
  summary.add_object("impairment", impairment);
  summary.add_object("generator", generator);
  summary.add_object("live_scene", live_scene);
  std::cout << "KCDS_LOAD_SUMMARY " << summary.str() << std::endl;
  if (!options.reference_capture.empty() && !reference_capture.completed()) {
    std::cerr << "reference-capture: no valid native Animations received; no file written to "
              << options.reference_capture << '\n';
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  load::LoadOptions options;
  try {
    std::vector<std::string> arguments(argv + 1, argv + argc);
    if (arguments.size() == 1 && (arguments[0] == "--help" || arguments[0] == "-h")) {
      std::cout << load::load_usage();
      return 0;
    }
    options = load::parse_load_options(arguments);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n' << load::load_usage();
    return 2;
  }
  try {
    return run(options);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
