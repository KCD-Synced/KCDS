#pragma once
// A kcds-bots run's result: result.json (schema 1, Docs: kcds-bots) and its time series, a CSV
// row every 5 seconds. The report generator (Dev/tools/bots/bots_report.py) reads both.
//
// Definitions the result relies on:
// - snapshot_age_ms: from a bot queueing its input (its MotionClock) to another bot of the same
//   process applying the snapshot that carries it; one clock, so it is the whole replication
//   delay (upload, the server's tick, snapshot, download), up to one bot tick of polling.
// - update_interval_ms by band: the time between two updates of another player as a bot saw
//   them, by its distance (near < 25 m, mid < 100 m, far beyond).
// - kbps: a bot's bytes over the time it played, per bot; avg/p50/p95/max over the bots.
// - server.tick_ms: p50 is the median of the server's interval p50s, p99 the median of its
//   interval p99s, max the highest interval max; missed ticks and egress are the change of the
//   server's own totals over the run.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "../../metrics/json_object.hpp"
#include "../bot/bot_stats.hpp"
#include "../scenario/scenario.hpp"
#include "machine.hpp"
#include "metrics_scrape.hpp"
#include "percentiles.hpp"

namespace kcd2s::local::bots {

inline constexpr int kResultSchema = 1;

// One row of the time series.
struct TimePoint {
  double elapsed_s = 0.0;
  std::size_t bots_playing = 0;
  std::size_t bots_failed = 0;
  double kbps_in = 0.0;  // all bots of this process together, over the last bucket
  double kbps_out = 0.0;
  std::uint64_t corrections = 0;  // so far
  std::uint64_t refused_joins = 0;
  double bot_cpu_percent = 0.0;  // of one core
  std::uint64_t bot_overruns = 0;
  ServerSample server;
};

inline std::string timeseries_header() {
  return "elapsed_s,bots_playing,bots_failed,kbps_in,kbps_out,corrections,refused_joins,"
         "bot_cpu_percent,bot_overruns,server_ok,tick_p50_ms,tick_p99_ms,tick_max_ms,"
         "missed_ticks,server_cpu_percent,server_rss_mb,host_cpu_percent,host_rss_mb,"
         "egress_kbps,players,bots,movement_refusals_total,melee_refusals_total\n";
}

inline std::string timeseries_row(const TimePoint& point) {
  char line[512];
  const ServerSample& s = point.server;
  std::snprintf(line, sizeof(line),
                "%.1f,%zu,%zu,%.1f,%.1f,%llu,%llu,%.1f,%llu,%d,%.3f,%.3f,%.3f,%.0f,%.1f,%.1f,"
                "%.1f,%.1f,%.1f,%.0f,%.0f,%.0f,%.0f\n",
                point.elapsed_s, point.bots_playing, point.bots_failed, point.kbps_in,
                point.kbps_out, (unsigned long long)point.corrections,
                (unsigned long long)point.refused_joins, point.bot_cpu_percent,
                (unsigned long long)point.bot_overruns, s.ok ? 1 : 0, s.tick_p50_ms, s.tick_p99_ms,
                s.tick_max_ms, s.missed_ticks, s.cpu_percent, s.rss_bytes / 1048576.0,
                s.host_cpu_percent, s.host_rss_bytes / 1048576.0, s.egress_bytes_per_s * 8 / 1000,
                s.players, s.bots, s.movement_refusals_total, s.melee_refusals_total);
  return line;
}

// One bot's numbers for the run's spread.
struct BotNumbers {
  std::string group;
  double kbps_in = 0.0;
  double kbps_out = 0.0;
  double corrections_per_min = 0.0;
};

struct RunFacts {
  std::string scenario;
  std::uint64_t seed = 0;
  std::string kcds_version;
  std::string git_sha;
  std::string started_at;  // ISO 8601, UTC
  double duration_s = 0.0;
  bool interrupted = false;
  std::string server_host;
  std::string server_hardware;
  unsigned machines = 1;
  std::string share;  // "1/2"
  NetworkConditions network;
  MachineInfo bot_machine;
  unsigned threads = 0;
  double tick_hz = 60.0;
  std::uint64_t ticks = 0;
  std::uint64_t overruns = 0;
  double bot_cpu_percent = 0.0;  // of one core, over the run
  double bot_rss_mb = 0.0;
  std::string timeseries_file;
};

inline std::string utc_now_iso() {
  const std::time_t now = std::time(nullptr);
  std::tm parts{};
#ifdef _WIN32
  gmtime_s(&parts, &now);
#else
  gmtime_r(&now, &parts);
#endif
  char text[32];
  std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &parts);
  return text;
}

namespace result_detail {

inline metrics::JsonObject summary_object(const Summary& summary) {
  metrics::JsonObject object;
  object.add_number("avg", summary.mean);
  object.add_number("p50", summary.p50);
  object.add_number("p95", summary.p95);
  object.add_number("max", summary.max);
  return object;
}

inline metrics::JsonObject histogram_object(const metrics::DurationHistogram& histogram) {
  metrics::JsonObject object;
  object.add_count("count", histogram.count());
  object.add_number("p50", histogram.percentile(50.0));
  object.add_number("p95", histogram.percentile(95.0));
  object.add_number("p99", histogram.percentile(99.0));
  object.add_number("max", histogram.max());
  return object;
}

inline double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return percentile_of_sorted(values, 50.0);
}

}  // namespace result_detail

// What several engines' results are merged from exactly (the npm CLI runs one engine per share of
// a machine's cores): every bot's numbers, the histograms' buckets (bounds:
// metrics::DurationHistogram) and the counters.
inline metrics::JsonObject merge_object(const RunFacts& facts, const BotStats& total,
                                        const std::vector<BotNumbers>& bots) {
  using metrics::JsonObject;
  std::string list = "[";
  for (std::size_t index = 0; index < bots.size(); ++index) {
    JsonObject bot;
    bot.add_text("group", bots[index].group);
    bot.add_number("kbps_in", bots[index].kbps_in);
    bot.add_number("kbps_out", bots[index].kbps_out);
    bot.add_number("corrections_per_min", bots[index].corrections_per_min);
    list += (index ? "," : "") + bot.str();
  }
  list += "]";
  JsonObject histograms;
  histograms.add_object("snapshot_age_ms", metrics::histogram_buckets(total.snapshot_age_ms));
  histograms.add_object("connect_to_spawn_ms",
                        metrics::histogram_buckets(total.connect_to_spawn_ms));
  for (std::size_t band = 0; band < total.update_interval_ms.size(); ++band) {
    histograms.add_object(std::string("update_interval_ms_") + kBandNames[band],
                          metrics::histogram_buckets(total.update_interval_ms[band]));
  }
  JsonObject counters;
  counters.add_count("join_attempts", total.join_attempts);
  counters.add_count("spawned", total.spawned);
  counters.add_count("refused_joins", total.refused_joins);
  counters.add_count("timed_out_joins", total.timed_out_joins);
  counters.add_count("corrections", total.corrections);
  counters.add_count("inputs", total.inputs);
  counters.add_count("chat_lines", total.chat_lines);
  counters.add_count("emotes", total.emotes);
  counters.add_count("swings", total.swings);
  counters.add_count("contacts_sent", total.contacts_sent);
  counters.add_count("contacts_received", total.contacts_received);
  counters.add_count("blocks", total.blocks);
  counters.add_count("effects_sent", total.effects_sent);
  counters.add_count("shots", total.shots);
  counters.add_count("voice_frames", total.voice_frames);
  counters.add_count("horses", total.horses);
  counters.add_count("mounts", total.mounts);
  counters.add_count("leaves", total.leaves);
  JsonObject reasons;
  for (const auto& [reason, count] : total.refusal_reasons) {
    reasons.add_count(reason, count);
  }
  JsonObject generator;
  generator.add_count("ticks", facts.ticks);
  generator.add_count("overruns", facts.overruns);
  generator.add_number("cpu_pct_of_one_core", facts.bot_cpu_percent);
  generator.add_count("threads", facts.threads);
  JsonObject merge;
  merge.add_raw("bots", list);
  merge.add_object("histograms", histograms);
  merge.add_object("counters", counters);
  merge.add_object("refusal_reasons", reasons);
  merge.add_object("generator", generator);
  return merge;
}

// result.json's text.
inline std::string result_json(const RunFacts& facts, const BotStats& total,
                               const std::vector<BotNumbers>& bots,
                               const std::vector<ServerSample>& samples) {
  using metrics::JsonObject;
  using result_detail::histogram_object;
  using result_detail::summary_object;

  JsonObject network;
  char latency[64];
  std::snprintf(latency, sizeof(latency), "[%.0f,%.0f]", facts.network.latency_ms.min,
                facts.network.latency_ms.max);
  network.add_raw("latency_ms", latency);
  network.add_number("jitter_ms", facts.network.jitter_ms);
  network.add_number("loss_pct", facts.network.loss * 100.0);

  JsonObject bot_machine;
  bot_machine.add_text("cpu", facts.bot_machine.cpu);
  bot_machine.add_count("threads", facts.bot_machine.threads);
  bot_machine.add_number("ram_gb", std::round(facts.bot_machine.ram_gb * 10.0) / 10.0);
  bot_machine.add_text("system", facts.bot_machine.system);

  JsonObject environment;
  environment.add_text("server_host", facts.server_host);
  environment.add_text("server_hardware", facts.server_hardware);
  environment.add_object("bot_machine", bot_machine);
  environment.add_count("bot_machines", facts.machines);
  environment.add_text("share", facts.share);
  environment.add_object("network", network);

  // The server, from its /metrics.
  std::vector<double> p50s;
  std::vector<double> p99s;
  std::vector<double> cpus;
  std::vector<double> host_cpus;
  double tick_max = 0.0;
  double cpu_max = 0.0;
  double rss_max = 0.0;
  double host_rss_max = 0.0;
  const ServerSample* first = nullptr;
  const ServerSample* last = nullptr;
  for (const ServerSample& sample : samples) {
    if (!sample.ok) {
      continue;
    }
    if (!first) {
      first = &sample;
    }
    last = &sample;
    if (sample.interval_s > 0.0) {
      p50s.push_back(sample.tick_p50_ms);
      p99s.push_back(sample.tick_p99_ms);
      cpus.push_back(sample.cpu_percent);
    }
    host_cpus.push_back(sample.host_cpu_percent);
    tick_max = std::max(tick_max, sample.tick_max_ms);
    cpu_max = std::max(cpu_max, sample.cpu_percent);
    rss_max = std::max(rss_max, sample.rss_bytes);
    host_rss_max = std::max(host_rss_max, sample.host_rss_bytes);
  }
  JsonObject tick;
  tick.add_number("p50", result_detail::median(p50s));
  tick.add_number("p99", result_detail::median(p99s));
  tick.add_number("max", tick_max);
  JsonObject cpu;
  cpu.add_number("avg", cpus.empty() ? 0.0 : summarize(cpus).mean);
  cpu.add_number("max", cpu_max);
  JsonObject rss;
  rss.add_number("start", first ? first->rss_bytes / 1048576.0 : 0.0);
  rss.add_number("end", last ? last->rss_bytes / 1048576.0 : 0.0);
  rss.add_number("max", rss_max / 1048576.0);
  JsonObject host;
  host.add_number("cpu_pct_avg", host_cpus.empty() ? 0.0 : summarize(host_cpus).mean);
  host.add_number("rss_mb_start", first ? first->host_rss_bytes / 1048576.0 : 0.0);
  host.add_number("rss_mb_end", last ? last->host_rss_bytes / 1048576.0 : 0.0);
  host.add_number("rss_mb_max", host_rss_max / 1048576.0);
  JsonObject refused_checks;
  refused_checks.add_number(
      "movement",
      first && last ? last->movement_refusals_total - first->movement_refusals_total : 0.0);
  refused_checks.add_number(
      "melee", first && last ? last->melee_refusals_total - first->melee_refusals_total : 0.0);
  const double span = first && last ? last->elapsed_s - first->elapsed_s : 0.0;
  JsonObject server;
  server.add_flag("scraped", first != nullptr);
  server.add_count("samples", samples.size());
  server.add_object("tick_ms", tick);
  server.add_number("missed_ticks",
                    first && last ? last->missed_ticks_total - first->missed_ticks_total : 0.0);
  server.add_object("cpu_pct", cpu);
  server.add_object("rss_mb", rss);
  server.add_object("resource_host", host);
  server.add_number(
      "egress_kbps",
      span > 0.0 ? (last->bytes_out_total - first->bytes_out_total) * 8.0 / 1000.0 / span : 0.0);
  double players_max = 0.0;
  for (const ServerSample& sample : samples) {
    players_max = std::max(players_max, sample.players);
  }
  server.add_number("players_max", players_max);
  server.add_object("refused_checks", refused_checks);

  // The bots.
  std::vector<double> kbps_in;
  std::vector<double> kbps_out;
  std::vector<double> corrections;
  for (const BotNumbers& bot : bots) {
    kbps_in.push_back(bot.kbps_in);
    kbps_out.push_back(bot.kbps_out);
    corrections.push_back(bot.corrections_per_min);
  }
  JsonObject bands;
  for (std::size_t band = 0; band < total.update_interval_ms.size(); ++band) {
    bands.add_object(kBandNames[band], histogram_object(total.update_interval_ms[band]));
  }
  JsonObject refusals;
  for (const auto& [reason, count] : total.refusal_reasons) {
    refusals.add_count(reason, count);
  }
  JsonObject actions;
  actions.add_count("inputs", total.inputs);
  actions.add_count("chat_lines", total.chat_lines);
  actions.add_count("emotes", total.emotes);
  actions.add_count("swings", total.swings);
  actions.add_count("contacts_sent", total.contacts_sent);
  actions.add_count("contacts_received", total.contacts_received);
  actions.add_count("blocks", total.blocks);
  actions.add_count("effects_sent", total.effects_sent);
  actions.add_count("shots", total.shots);
  actions.add_count("voice_frames", total.voice_frames);
  actions.add_count("horses", total.horses);
  actions.add_count("mounts", total.mounts);
  actions.add_count("leaves", total.leaves);
  JsonObject connect;
  connect.add_number("p50", total.connect_to_spawn_ms.percentile(50.0));
  connect.add_number("p95", total.connect_to_spawn_ms.percentile(95.0));
  connect.add_number("max", total.connect_to_spawn_ms.max());
  JsonObject clients;
  clients.add_count("bots", bots.size());
  clients.add_count("joins", total.join_attempts);
  clients.add_count("spawned", total.spawned);
  clients.add_object("kbps_in", summary_object(summarize(kbps_in)));
  clients.add_object("kbps_out", summary_object(summarize(kbps_out)));
  clients.add_object("snapshot_age_ms", histogram_object(total.snapshot_age_ms));
  clients.add_object("update_interval_ms", bands);
  clients.add_number("corrections_per_min", summarize(corrections).mean);
  clients.add_count("corrections", total.corrections);
  clients.add_object("connect_to_spawn_ms", connect);
  clients.add_count("failed_connects", total.refused_joins + total.timed_out_joins);
  clients.add_count("refused_connects", total.refused_joins);
  clients.add_count("timed_out_connects", total.timed_out_joins);
  clients.add_object("refusal_reasons", refusals);
  clients.add_object("actions", actions);

  JsonObject generator;
  generator.add_count("threads", facts.threads);
  generator.add_number("tick_hz", facts.tick_hz);
  generator.add_count("ticks", facts.ticks);
  generator.add_count("overruns", facts.overruns);
  generator.add_number("overrun_pct",
                       facts.ticks ? 100.0 * double(facts.overruns) / double(facts.ticks) : 0.0);
  generator.add_number("cpu_pct_of_one_core", facts.bot_cpu_percent);
  generator.add_number("cpu_pct_of_machine", facts.bot_machine.threads
                                                 ? facts.bot_cpu_percent / facts.bot_machine.threads
                                                 : 0.0);
  generator.add_number("rss_mb", facts.bot_rss_mb);

  JsonObject result;
  result.add_integer("schema", kResultSchema);
  result.add_text("kcds_version", facts.kcds_version);
  result.add_text("git_sha", facts.git_sha);
  result.add_text("scenario", facts.scenario);
  result.add_count("seed", facts.seed);
  result.add_text("started_at", facts.started_at);
  result.add_number("duration_s", facts.duration_s);
  result.add_flag("interrupted", facts.interrupted);
  result.add_object("environment", environment);
  result.add_object("server", server);
  result.add_object("clients", clients);
  result.add_object("generator", generator);
  result.add_text("timeseries", facts.timeseries_file);
  result.add_object("merge", merge_object(facts, total, bots));
  return result.str();
}

}  // namespace kcd2s::local::bots
