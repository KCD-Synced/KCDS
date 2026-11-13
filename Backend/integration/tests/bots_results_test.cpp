// kcds-bots results (local/bots/results, cli): percentiles, the server's /metrics, the command
// line and result.json.
#include "../local/bots/bot/bot.hpp"
#include "../local/bots/cli/bots_options.hpp"
#include "../local/bots/results/run_result.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "kcd2s/testkit.hpp"

using namespace kcd2s::local::bots;

namespace {

bool contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

// The option parse's error text, or "" when it parses.
std::string option_error(const std::vector<std::string>& arguments) {
  try {
    parse_bots_options(arguments);
  } catch (const std::exception& error) {
    return error.what();
  }
  return "";
}

}  // namespace

KCD2S_TEST(bots_results, nearest_rank_percentiles) {
  std::vector<double> values;
  for (int i = 100; i >= 1; --i) {
    values.push_back(double(i));
  }
  const Summary summary = summarize(values);
  KCD2S_CHECK_EQ(summary.count, std::size_t{100});
  KCD2S_CHECK_EQ(summary.p50, 50.0);
  KCD2S_CHECK_EQ(summary.p95, 95.0);
  KCD2S_CHECK_EQ(summary.p99, 99.0);
  KCD2S_CHECK_EQ(summary.max, 100.0);
  KCD2S_CHECK_EQ(summary.mean, 50.5);
  KCD2S_CHECK_EQ(summarize({}).p95, 0.0);
  KCD2S_CHECK_EQ(summarize({7.0}).p50, 7.0);
}

KCD2S_TEST(bots_results, reads_the_servers_prometheus_text) {
  const auto metrics = parse_prometheus(
      "# TYPE kcds_players gauge\n"
      "kcds_players 12\n"
      "kcds_players_by_kind{kind=\"bot\"} 10\n"
      "kcds_tick_work_ms{stat=\"p99\"} 7.5\n"
      "kcds_tick_work_ms{stat=\"p50\"} 5\n"
      "kcds_tick_missed_deadlines 2\n"
      "kcds_since_start_missed_deadlines 9\n"
      "kcds_interval_s 5\n"
      "kcds_udp_interval_bytes_out 5000000\n"
      "kcds_udp_total_bytes_out 123456789\n"
      "kcds_process_cpu_percent_of_one_core 198.4\n"
      "kcds_host_rss_bytes 330000000\n"
      "kcds_checks_movement_refusals_total 3\n"
      "not a metric line\n");
  const ServerSample sample = read_server_sample(metrics);
  KCD2S_CHECK(sample.ok);
  KCD2S_CHECK_EQ(sample.players, 12.0);
  KCD2S_CHECK_EQ(sample.bots, 10.0);
  KCD2S_CHECK_EQ(sample.tick_p99_ms, 7.5);
  KCD2S_CHECK_EQ(sample.missed_ticks_total, 9.0);
  KCD2S_CHECK_EQ(sample.egress_bytes_per_s, 1000000.0);
  KCD2S_CHECK_EQ(sample.movement_refusals_total, 3.0);
  KCD2S_CHECK(!read_server_sample(parse_prometheus("")).ok);
}

KCD2S_TEST(bots_results, metrics_urls) {
  const auto url = parse_http_url("http://192.168.1.20:7791/metrics");
  KCD2S_CHECK(url && url->host == "192.168.1.20" && url->port == 7791 && url->path == "/metrics");
  const auto bare = parse_http_url("http://server");
  KCD2S_CHECK(bare && bare->port == 80 && bare->path == "/");
  KCD2S_CHECK(!parse_http_url("https://server/metrics"));
  KCD2S_CHECK(!parse_http_url("http://server:99999/"));
  KCD2S_CHECK(!parse_http_url("http://:80/"));
}

KCD2S_TEST(bots_results, the_command_line) {
  const BotsOptions options = parse_bots_options(
      {"--server", "10.0.0.5:7790", "--scenario", "town.yaml", "--out", "r.json", "--bots", "120",
       "--threads", "4", "--seed", "9", "--duration", "3m", "--share", "2/2"});
  KCD2S_CHECK_EQ(options.host, std::string("10.0.0.5"));
  KCD2S_CHECK_EQ(options.port, static_cast<unsigned short>(7790));
  KCD2S_CHECK_EQ(*options.bots, 120u);
  KCD2S_CHECK_EQ(*options.duration_s, 180.0);
  KCD2S_CHECK(options.share_index == 2 && options.share_count == 2);
  // A share is not a machine: the npm CLI runs several engines on one; it says how many machines.
  KCD2S_CHECK_EQ(options.machines, 1u);
  KCD2S_CHECK(contains(option_error({"--scenario", "a", "--out", "b"}), "--server is required"));
  KCD2S_CHECK(contains(option_error({"--server", "host"}), "host:port"));
  KCD2S_CHECK(
      contains(option_error({"--server", "h:1", "--scenario", "a", "--out", "b", "--share", "3/2"}),
               "--share I"));
  KCD2S_CHECK(contains(option_error({"--bogus"}), "unknown option --bogus"));
}

KCD2S_TEST(bots_results, scaling_keeps_the_groups_proportions) {
  Scenario scenario = ScenarioReader().read(
      "name: s\ngroups:\n"
      "  - { name: a, count: 100, behavior: idle }\n"
      "  - { name: b, count: 50, behavior: idle }\n"
      "  - { name: c, count: 1, behavior: idle }\n");
  scale_groups(scenario, 30);
  KCD2S_CHECK_EQ(scenario.bot_count(), 30u);
  KCD2S_CHECK_EQ(scenario.groups[0].count, 20u);
  KCD2S_CHECK_EQ(scenario.groups[1].count, 9u);
  KCD2S_CHECK(scenario.groups[2].count >= 1u);
}

KCD2S_TEST(bots_results, result_json_has_the_schema_fields) {
  RunFacts facts;
  facts.scenario = "battle";
  facts.seed = 42;
  facts.kcds_version = "1.0.6";
  facts.git_sha = "abc";
  facts.started_at = "2026-11-12T10:00:00Z";
  facts.duration_s = 600;
  facts.network.latency_ms = {40, 120};
  facts.network.jitter_ms = 15;
  facts.network.loss = 0.01;
  facts.ticks = 1000;
  facts.overruns = 5;
  facts.timeseries_file = "battle.timeseries.csv";
  BotStats total;
  total.join_attempts = 2;
  total.spawned = 2;
  total.refused_joins = 1;
  total.refusal_reasons["This server does not accept bots."] = 1;
  total.snapshot_age_ms.add(30.0);
  total.connect_to_spawn_ms.add(310.0);
  std::vector<BotNumbers> bots{{"red", 100.0, 50.0, 0.0}, {"blue", 200.0, 60.0, 1.0}};
  std::vector<ServerSample> samples(2);
  samples[0].ok = true;
  samples[0].interval_s = 5;
  samples[0].tick_p99_ms = 4;
  samples[0].bytes_out_total = 1000;
  samples[0].missed_ticks_total = 1;
  samples[1] = samples[0];
  samples[1].elapsed_s = 10;
  samples[1].bytes_out_total = 1000 + 125000 * 10;  // 1000 kbps for 10 s
  samples[1].missed_ticks_total = 4;
  samples[1].movement_refusals_total = 2;
  const std::string json = result_json(facts, total, bots, samples);
  KCD2S_CHECK(contains(json, "\"schema\":1"));
  KCD2S_CHECK(contains(json, "\"kcds_version\":\"1.0.6\""));
  KCD2S_CHECK(contains(json, "\"scenario\":\"battle\""));
  KCD2S_CHECK(contains(json, "\"latency_ms\":[40,120]"));
  KCD2S_CHECK(contains(json, "\"loss_pct\":1"));
  KCD2S_CHECK(contains(json, "\"missed_ticks\":3"));
  KCD2S_CHECK(contains(json, "\"egress_kbps\":1000"));
  KCD2S_CHECK(contains(json, "\"refused_checks\":{\"movement\":2,\"melee\":0}"));
  KCD2S_CHECK(contains(json, "\"failed_connects\":1"));
  KCD2S_CHECK(contains(json, "\"This server does not accept bots.\":1"));
  KCD2S_CHECK(contains(json, "\"kbps_in\":{\"avg\":150"));
  KCD2S_CHECK(contains(json, "\"overrun_pct\":0.5"));
  KCD2S_CHECK(contains(json, "\"timeseries\":\"battle.timeseries.csv\""));
  const std::string row = timeseries_row(TimePoint{});
  const std::string header = timeseries_header();
  KCD2S_CHECK_EQ(std::count(row.begin(), row.end(), ','),
                 std::count(header.begin(), header.end(), ','));
}

KCD2S_TEST(bots_results, bot_nicknames_carry_group_and_run) {
  KCD2S_CHECK_EQ(kcd2s::local::bots::bot_nickname("red", 12, "ab3k"),
                 std::string("bot-red-12.ab3k"));
  const auto parts = kcd2s::local::bots::parse_bot_nickname("bot-fast-riders-3.ab3k");
  KCD2S_CHECK(parts.bot && parts.group == "fast-riders" && parts.run_tag == "ab3k");
  KCD2S_CHECK(!kcd2s::local::bots::parse_bot_nickname("Henry").bot);
  KCD2S_CHECK(!kcd2s::local::bots::parse_bot_nickname("bot-red").bot);
}
