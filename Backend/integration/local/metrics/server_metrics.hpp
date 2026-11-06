#pragma once
// Opt-in kcd2s-server metrics (measurement tooling). Disabled unless the environment
// variable KCDS_SERVER_METRICS holds a positive reporting interval in seconds. When enabled, the
// server prints one machine-readable line per interval:
//
//   KCDS_METRICS {"schema":"kcds.server.metrics.v1", ...}
//
// Tick "work" is the duration of Host::tick(). The loop runs a fixed-rate schedule (see
// tick/fixed_rate_schedule.hpp): ticks start at absolute deadlines one tick interval apart.
// - missed_deadlines: ticks whose work exceeded the tick interval (same definition as the first
//   baseline, which used a nominal 16 ms interval).
// - late_finishes: ticks that ended after the next tick's deadline (start lateness + work).
// - skipped_slots: whole tick slots dropped because the loop was a full interval or more behind.
// - start_lateness: how long after its deadline each tick started (timer wake-up error + backlog).
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "../runtime.hpp"
#include "counters.hpp"
#include "duration_histogram.hpp"
#include "json_object.hpp"
#include "metrics_settings.hpp"
#ifdef _WIN32
#include "process_usage_win.hpp"
#else
#include "process_usage_posix.hpp"
#endif
#include "../tick/fixed_rate_schedule.hpp"

namespace kcd2s::local::metrics {

inline std::optional<double> metrics_interval_from_environment() {
#ifdef _WIN32
  char* value = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&value, &length, "KCDS_SERVER_METRICS") != 0 || !value) {
    return std::nullopt;
  }
  const std::string text(value);
  std::free(value);
#else
  const char* value = std::getenv("KCDS_SERVER_METRICS");
  if (!value) {
    return std::nullopt;
  }
  const std::string text(value);
#endif
  return parse_metrics_interval(text.c_str());
}

class ServerMetricsReporter {
 public:
  // `tick_rate_hz` is the configured server tick rate; `scheduler` describes the wait used.
  ServerMetricsReporter(double interval_seconds, double tick_rate_hz, std::string scheduler)
      : interval_seconds_(interval_seconds),
        tick_interval_ms_(1000.0 / tick_rate_hz),
        tick_rate_hz_(tick_rate_hz),
        scheduler_(std::move(scheduler)) {
  }

  // One-time configuration line so a report can record the settings the server actually ran with.
  // The limits below mirror the literals in Host (runtime.hpp); keep them in sync.
  void print_configuration(const std::string& map, const config::HostSettings& settings,
                           const Host& host) const {
    JsonObject line;
    line.add_text("schema", "kcds.server.metrics-config.v1");
    line.add_integer("unix_ms", unix_milliseconds());
    line.add_number("interval_s", interval_seconds_);
    line.add_number("nominal_tick_interval_ms", tick_interval_ms_);
    line.add_number("tick_rate_hz", tick_rate_hz_);
    line.add_text("tick_scheduler", scheduler_);
    line.add_count("peer_cap", settings.max_peers);
    line.add_text("over_capacity_handshake",
                  "explicit CONNECT_REFUSED (server full), rate limited");
    line.add_number("interest_radius", settings.interest_radius);
    line.add_number("interest_hysteresis", settings.interest_hysteresis);
    line.add_count("max_characters", settings.max_characters);
    line.add_text("interest_plane", "x/y (z up)");
    line.add_number("interest_cell_size", settings.interest_cell_size());
    line.add_flag("parallel_snapshot_builder", settings.parallel_snapshots());
    line.add_count("snapshot_workers",
                   settings.parallel_snapshots() ? settings.snapshot_workers : 0);
    const integration::SnapshotBudget budget = settings.snapshot_budget();
    line.add_count("peer_snapshot_bytes_per_s", settings.peer_snapshot_bytes_per_second);
    line.add_count("total_snapshot_bytes_per_s", settings.total_snapshot_bytes_per_second);
    line.add_count("peer_snapshot_bytes_per_tick", budget.peer_bytes_per_tick);
    line.add_count("total_snapshot_bytes_per_tick", budget.total_bytes_per_tick);
    line.add_count("max_datagram_bytes", settings.max_datagram_bytes);
    line.add_count("receive_budget_per_tick", settings.receive_budget());
    line.add_integer("socket_receive_buffer_bytes", host.socket_receive_buffer_bytes());
    line.add_integer("socket_send_buffer_bytes", host.socket_send_buffer_bytes());
    line.add_count("member_inbox_limit", 256);
    line.add_number("idle_timeout_s", 10.0);
    line.add_text("map", map);
    line.add_count("logical_cores", logical_core_count());
    std::cout << "KCDS_METRICS_CONFIG " << line.str() << std::endl;
  }

  void start(double now, Host& host) {
    started_at_ = now;
    interval_started_at_ = now;
    next_report_ = now + interval_seconds_;
    previous_usage_ = sample_process_usage();
    previous_socket_ = host.socket_traffic();
    previous_budget_ = host.snapshot_budget_counters();
    previous_fragments_ = host.queue_view();
    host.enable_phase_timing();
  }

  // Call around every Host::tick() with clock_seconds() readings and the schedule's slot.
  void record_tick(double tick_start, double tick_end, const tick::TickSlot& slot, Host& host) {
    const double work_ms = (tick_end - tick_start) * 1000.0;
    interval_work_.add(work_ms);
    total_work_.add(work_ms);
    interval_lateness_.add(slot.lateness * 1000.0);
    if (work_ms > tick_interval_ms_) {
      interval_missed_ += 1;
      total_missed_ += 1;
    }
    const double next_deadline = slot.deadline + tick_interval_ms_ / 1000.0;
    if (tick_end > next_deadline) {
      interval_late_finishes_ += 1;
      total_late_finishes_ += 1;
    }
    interval_skipped_ += slot.skipped;
    total_skipped_ += slot.skipped;
    if (last_tick_start_ > 0.0) {
      interval_period_.add((tick_start - last_tick_start_) * 1000.0);
    }
    last_tick_start_ = tick_start;
    if (tick_end >= next_report_) {
      report(tick_end, host);
    }
  }

 private:
  void report(double now, Host& host) {
    const ProcessUsage usage = sample_process_usage();
    const TrafficCounters socket_total = host.socket_traffic();
    const TrafficCounters socket_interval = traffic_delta(socket_total, previous_socket_);
    const HostQueueView queues = host.queue_view();
    const HostCounters& counters = host.counters();
    HostPhaseTimes& phases = host.phase_times();

    JsonObject tick;
    tick.add_count("ticks", interval_work_.count());
    tick.add_object("work", describe(interval_work_));
    tick.add_object("period", describe(interval_period_));
    tick.add_count("missed_deadlines", interval_missed_);
    tick.add_count("late_finishes", interval_late_finishes_);
    tick.add_count("skipped_slots", interval_skipped_);
    tick.add_object("start_lateness", describe(interval_lateness_));
    JsonObject phase;
    phase.add_object("ingress", describe(phases.ingress));
    phase.add_object("timeouts", describe(phases.timeouts));
    phase.add_object("server", describe(phases.server));
    phase.add_object("server_receive", describe(phases.server_receive));
    phase.add_object("snapshot_build", describe(phases.snapshot_build));
    phase.add_object("snapshot_send", describe(phases.snapshot_send));
    phase.add_object("resources", describe(phases.resources));
    tick.add_object("phases", phase);

    JsonObject totals;
    totals.add_object("work", describe(total_work_));
    totals.add_count("missed_deadlines", total_missed_);
    totals.add_count("late_finishes", total_late_finishes_);
    totals.add_count("skipped_slots", total_skipped_);

    JsonObject peers;
    peers.add_count("connected", queues.peers);
    peers.add_count("joins_total", counters.joins);
    peers.add_count("leaves_total", counters.leaves);
    peers.add_count("refused_handshake_datagrams_total", counters.handshakes_refused_at_capacity);
    peers.add_count("refusals_sent_total", counters.refusals_sent);
    peers.add_count("refusals_rate_limited_total", counters.refusals_rate_limited);

    JsonObject udp;
    udp.add_object("interval", traffic_object(socket_interval));
    udp.add_object("total", traffic_object(socket_total));

    JsonObject queue;
    queue.add_count("inbox_total", queues.inbox_total);
    queue.add_count("inbox_max", queues.inbox_max);
    queue.add_count("outstanding_reliable_total", queues.outstanding_reliable_total);
    queue.add_count("reliable_sent_total", queues.reliable_sent_total);
    queue.add_count("reliable_retransmits_total", queues.reliable_retransmits_total);
    queue.add_count("inbox_overflow_drops_total", counters.inbox_overflow_drops);
    queue.add_count("receive_budget_exhausted_ticks_total",
                    counters.receive_budget_exhausted_ticks);
    queue.add_count("receive_thread_drops_total", host.receive_thread_drops());

    // Snapshot byte budget outcome in this interval (phase 2).
    const integration::SnapshotBudgetCounters budget = host.snapshot_budget_counters();
    JsonObject snapshots;
    snapshots.add_count("built", budget.snapshots - previous_budget_.snapshots);
    snapshots.add_count("payload_bytes", budget.snapshot_bytes - previous_budget_.snapshot_bytes);
    snapshots.add_count("stride", budget.snapshot_stride);
    snapshots.add_count("budget_limited", budget.budget_limited_snapshots -
                                              previous_budget_.budget_limited_snapshots);
    snapshots.add_count("deferred_entries",
                        budget.deferred_entries - previous_budget_.deferred_entries);
    snapshots.add_count("deferred_constructs",
                        budget.deferred_constructs - previous_budget_.deferred_constructs);
    snapshots.add_count("overdraw",
                        budget.overdraw_snapshots - previous_budget_.overdraw_snapshots);
    snapshots.add_count("aggregate_limited_ticks",
                        budget.aggregate_limited_ticks - previous_budget_.aggregate_limited_ticks);
    snapshots.add_count("largest_payload_bytes_since_start", budget.largest_snapshot_bytes);
    snapshots.add_count("update_cache_hits",
                        budget.update_cache_hits - previous_budget_.update_cache_hits);
    snapshots.add_count("update_cache_misses",
                        budget.update_cache_misses - previous_budget_.update_cache_misses);
    snapshots.add_count("update_cache_fallbacks",
                        budget.update_cache_fallbacks - previous_budget_.update_cache_fallbacks);
    snapshots.add_count(
        "update_cache_busy_fallbacks",
        budget.update_cache_busy_fallbacks - previous_budget_.update_cache_busy_fallbacks);
    snapshots.add_count(
        "update_cache_variant_fallbacks",
        budget.update_cache_variant_fallbacks - previous_budget_.update_cache_variant_fallbacks);
    snapshots.add_count(
        "update_cache_memory_fallbacks",
        budget.update_cache_memory_fallbacks - previous_budget_.update_cache_memory_fallbacks);
    snapshots.add_count(
        "update_cache_phase_fallbacks",
        budget.update_cache_phase_fallbacks - previous_budget_.update_cache_phase_fallbacks);
    snapshots.add_count("fragmented_datagrams", queues.fragmented_datagrams_total -
                                                    previous_fragments_.fragmented_datagrams_total);
    snapshots.add_count("fragments", queues.fragments_total - previous_fragments_.fragments_total);
    snapshots.add_count("largest_udp_payload_since_start", socket_total.largest_sent);

    const auto character_counts = host.character_admission_totals();
    JsonObject characters;
    characters.add_count("candidates", character_counts.candidates);
    characters.add_count("admitted", character_counts.admitted);
    characters.add_count("resident", character_counts.resident);
    characters.add_count("retiring", character_counts.retiring);
    characters.add_count("deferred", character_counts.deferred);

    // Resource host bridge in this interval: IPC bytes the simulation thread wrote, the time it
    // waited on the pipe, and the size of the delta-tracked views.
    const HostResourceView resource = host.resource_view();
    if (resource.bridge.messages_sent < previous_bridge_.messages_sent) {
      // A new resource host started: its counters begin at zero.
      previous_bridge_ = ScriptBridgeCounters{};
    }
    JsonObject resources;
    resources.add_flag("running", resource.running);
    resources.add_count("messages_to_host",
                        resource.bridge.messages_sent - previous_bridge_.messages_sent);
    resources.add_count("bytes_to_host", resource.bridge.bytes_sent - previous_bridge_.bytes_sent);
    resources.add_number("write_wait_ms",
                         (resource.bridge.write_seconds - previous_bridge_.write_seconds) * 1000.0);
    resources.add_count("write_queue_bytes", resource.bridge.queued_bytes);
    resources.add_count("messages_from_host",
                        resource.bridge.messages_received - previous_bridge_.messages_received);
    resources.add_count("inbox_drops_total", resource.bridge.inbox_drops);
    resources.add_count("tracked_entities", resource.tracked_entities);
    resources.add_count("tracked_views", resource.tracked_views);

    JsonObject process;
    process.add_number("cpu_percent_of_one_core", cpu_percent_of_one_core(previous_usage_, usage));
    process.add_count("logical_cores", logical_core_count());
    process.add_count("working_set_bytes", usage.working_set_bytes);
    process.add_count("peak_working_set_bytes", usage.peak_working_set_bytes);

    JsonObject line;
    line.add_text("schema", "kcds.server.metrics.v1");
    line.add_integer("unix_ms", unix_milliseconds());
    line.add_number("uptime_s", now - started_at_);
    line.add_number("interval_s", now - interval_started_at_);
    line.add_object("tick", tick);
    line.add_object("since_start", totals);
    line.add_object("peers", peers);
    line.add_object("udp", udp);
    line.add_object("per_peer_interval", per_peer_object(queues));
    line.add_object("queues", queue);
    line.add_count("snapshot_payload_bytes_total", queues.snapshot_bytes_total);
    line.add_object("snapshots", snapshots);
    line.add_object("characters", characters);
    line.add_object("resources", resources);
    line.add_object("process", process);
    std::cout << "KCDS_METRICS " << line.str() << std::endl;

    interval_work_.reset();
    interval_period_.reset();
    interval_lateness_.reset();
    interval_missed_ = 0;
    interval_late_finishes_ = 0;
    interval_skipped_ = 0;
    phases.reset();
    previous_usage_ = usage;
    previous_socket_ = socket_total;
    previous_budget_ = budget;
    previous_fragments_ = queues;
    previous_bridge_ = resource.bridge;
    interval_started_at_ = now;
    while (next_report_ <= now) {
      next_report_ += interval_seconds_;
    }
  }

  static JsonObject traffic_object(const TrafficCounters& traffic) {
    JsonObject object;
    object.add_count("packets_in", traffic.packets_received);
    object.add_count("bytes_in", traffic.bytes_received);
    object.add_count("packets_out", traffic.packets_sent);
    object.add_count("bytes_out", traffic.bytes_sent);
    object.add_count("send_would_block", traffic.send_would_block);
    object.add_count("send_dropped", traffic.send_dropped);
    return object;
  }

  // Interval bytes per connected peer (min/mean/max) from each member's own link counters.
  // Peers that joined during the interval count from zero.
  JsonObject per_peer_object(const HostQueueView& queues) {
    std::unordered_map<core::PeerId, HostPeerTraffic> current;
    std::uint64_t peers = 0;
    std::uint64_t out_min = 0;
    std::uint64_t out_max = 0;
    std::uint64_t out_sum = 0;
    std::uint64_t in_min = 0;
    std::uint64_t in_max = 0;
    std::uint64_t in_sum = 0;
    for (const auto& traffic : queues.peer_traffic) {
      HostPeerTraffic previous;
      const auto found = previous_peers_.find(traffic.peer);
      if (found != previous_peers_.end()) {
        previous = found->second;
      }
      const std::uint64_t out = traffic.bytes_sent - previous.bytes_sent;
      const std::uint64_t in = traffic.bytes_received - previous.bytes_received;
      if (peers == 0 || out < out_min) {
        out_min = out;
      }
      if (peers == 0 || in < in_min) {
        in_min = in;
      }
      if (out > out_max) {
        out_max = out;
      }
      if (in > in_max) {
        in_max = in;
      }
      out_sum += out;
      in_sum += in;
      peers += 1;
      current.emplace(traffic.peer, traffic);
    }
    previous_peers_ = std::move(current);
    JsonObject object;
    object.add_count("peers", peers);
    object.add_count("bytes_out_min", out_min);
    object.add_number("bytes_out_mean", peers ? double(out_sum) / double(peers) : 0.0);
    object.add_count("bytes_out_max", out_max);
    object.add_count("bytes_in_min", in_min);
    object.add_number("bytes_in_mean", peers ? double(in_sum) / double(peers) : 0.0);
    object.add_count("bytes_in_max", in_max);
    return object;
  }

  double interval_seconds_;
  double tick_interval_ms_;
  double tick_rate_hz_;
  std::string scheduler_;
  double started_at_ = 0.0;
  double interval_started_at_ = 0.0;
  double next_report_ = 0.0;
  double last_tick_start_ = 0.0;
  DurationHistogram interval_work_;
  DurationHistogram interval_period_;
  DurationHistogram interval_lateness_;
  DurationHistogram total_work_;
  std::uint64_t interval_missed_ = 0;
  std::uint64_t total_missed_ = 0;
  std::uint64_t interval_late_finishes_ = 0;
  std::uint64_t total_late_finishes_ = 0;
  std::uint64_t interval_skipped_ = 0;
  std::uint64_t total_skipped_ = 0;
  ProcessUsage previous_usage_;
  TrafficCounters previous_socket_;
  integration::SnapshotBudgetCounters previous_budget_;
  HostQueueView previous_fragments_;
  ScriptBridgeCounters previous_bridge_;
  std::unordered_map<core::PeerId, HostPeerTraffic> previous_peers_;
};

}  // namespace kcd2s::local::metrics
