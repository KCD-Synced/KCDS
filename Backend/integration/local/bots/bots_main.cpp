// kcds-bots: a headless load-testing client for a KCDS server. It runs a scenario's bots
// (scenario/, behaviours/, bot/) on a few threads (runner/), reads the server's /metrics while
// they play and writes result.json and its time series (results/). Docs: kcds-bots in KCDS-DOCS.
//
// The server must be a development build running with KCDS_ALLOW_BOTS=1: bots never join a
// published server (integration/bot_admission.hpp).
#include <atomic>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "bot/bot.hpp"
#include "cli/bots_options.hpp"
#include "kcd2s/integration/guid.hpp"
#include "results/run_result.hpp"
#include "runner/bot_threads.hpp"
#ifdef _WIN32
#include "../metrics/process_usage_win.hpp"
#else
#include "../metrics/process_usage_posix.hpp"
#endif

#ifndef KCDS_BOTS_VERSION
#define KCDS_BOTS_VERSION ""
#endif
#ifndef KCDS_BOTS_GIT_SHA
#define KCDS_BOTS_GIT_SHA ""
#endif

using namespace kcd2s::local;
using namespace kcd2s::local::bots;

namespace {

std::atomic<bool> stop_requested{false};

void request_stop(int) {
  stop_requested.store(true);
}

// The arrow the archers loose: arrow_crude of the server's item catalog
// (Backend/scripting/runtimes/js/inventory/catalog.json).
constexpr const char* kDefaultAmmo = "dfea5d01-b25c-414a-9ab4-6911a5f82118";

std::string random_tag() {
  std::random_device device;
  const char* const letters = "abcdefghijkmnpqrstuvwxyz23456789";
  std::string tag;
  for (int i = 0; i < 4; ++i) {
    tag.push_back(letters[device() % 32]);
  }
  return tag;
}

// This machine's bots of the scenario (every N-th of each group with --share I/N).
std::vector<std::unique_ptr<Bot>> make_bots(const Scenario& scenario, const BotsOptions& options,
                                            const RunShared& shared) {
  std::vector<std::unique_ptr<Bot>> bots;
  std::size_t index = 0;
  for (const Group& group : scenario.groups) {
    for (unsigned n = 0; n < group.count; ++n, ++index) {
      if (index % options.share_count != options.share_index - 1) {
        continue;
      }
      BotPlan plan;
      plan.index = index;
      plan.group = &group;
      const auto area = scenario.areas.find(group.area);
      plan.area = area == scenario.areas.end() ? nullptr : &area->second;
      plan.spawn_radius = group.spawn_radius;
      plan.network = group.network.value_or(scenario.network);
      BotRandom join(scenario.seed ^ 0x6A6F696Eull, index);
      if (scenario.join_window_s > 0.0) {
        plan.join_at = join.uniform(0.0, scenario.join_window_s);
      } else {
        // Joins spread over a moment, as players arrive: 20 a second.
        plan.join_at = double(index) / 20.0;
      }
      bots.push_back(std::make_unique<Bot>(plan, shared));
    }
  }
  return bots;
}

// The server's IPv4 address in numbers (the client takes no names); nullopt when it has none.
std::optional<std::string> numeric_address(const std::string& host) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* found = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || !found) {
    return std::nullopt;
  }
  char text[INET_ADDRSTRLEN] = {};
  const auto* address = reinterpret_cast<const sockaddr_in*>(found->ai_addr);
  inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text));
  freeaddrinfo(found);
  return std::string(text);
}

bool uses(const Scenario& scenario, const std::string& behaviour) {
  for (const Group& group : scenario.groups) {
    if (group.behavior == behaviour) {
      return true;
    }
  }
  return false;
}

int run(const BotsOptions& options) {
  Scenario scenario = read_scenario_file(options.scenario_path);
  if (options.bots) {
    scale_groups(scenario, *options.bots);
  }
  if (options.seed) {
    scenario.seed = *options.seed;
  }
  if (options.duration_s) {
    scenario.duration_s = *options.duration_s;
  }
  const auto problems = check_behaviours(scenario);
  if (!problems.empty()) {
    for (const auto& problem : problems) {
      std::cerr << options.scenario_path << ": " << problem << '\n';
    }
    return 2;
  }
  std::optional<HttpUrl> metrics_url;
  if (!options.metrics_url.empty()) {
    metrics_url = parse_http_url(options.metrics_url);
    if (!metrics_url) {
      std::cerr << "--metrics-url must be http://host:port/path\n";
      return 2;
    }
  }

  Winsock winsock;
  const auto address = numeric_address(options.host);
  if (!address) {
    std::cerr << "cannot find the address of " << options.host << '\n';
    return 2;
  }
  RunShared shared;
  shared.host = *address;
  shared.bind = address->rfind("127.", 0) == 0 ? "127.0.0.1" : "0.0.0.0";
  shared.port = options.port;
  shared.run_tag = random_tag();
  shared.seed = scenario.seed;
  kcd2s::integration::Guid ammo{};
  kcd2s::integration::parse_guid(kDefaultAmmo, ammo);
  std::copy(ammo.begin(), ammo.end(), shared.ammo.begin());
  std::unique_ptr<VoiceLoop> voice;
  if (uses(scenario, "voice")) {
    voice = std::make_unique<VoiceLoop>(scenario.seed);
    shared.voice = voice.get();
  }

  auto bots = make_bots(scenario, options, shared);
  const std::size_t bot_count = bots.size();
  const unsigned threads = options.threads.value_or(default_threads(bot_count));
  BotThreads runner(std::move(bots), threads, options.tick_hz);

  RunFacts facts;
  facts.scenario = scenario.name;
  facts.seed = scenario.seed;
  facts.kcds_version = KCDS_BOTS_VERSION;
  const char* sha = std::getenv("KCDS_GIT_SHA");
  facts.git_sha = sha && *sha ? sha : KCDS_BOTS_GIT_SHA;
  facts.started_at = utc_now_iso();
  facts.server_host = options.host + ":" + std::to_string(options.port);
  facts.server_hardware = options.server_hardware;
  facts.machines = options.machines;
  facts.share = std::to_string(options.share_index) + "/" + std::to_string(options.share_count);
  facts.network = scenario.network;
  facts.bot_machine = this_machine();
  facts.threads = unsigned(runner.thread_count());
  facts.tick_hz = options.tick_hz;
  std::string csv_path = options.out_path;
  if (csv_path.size() > 5 && csv_path.compare(csv_path.size() - 5, 5, ".json") == 0) {
    csv_path.resize(csv_path.size() - 5);
  }
  csv_path += ".timeseries.csv";
  const std::size_t slash = csv_path.find_last_of("/\\");
  facts.timeseries_file = slash == std::string::npos ? csv_path : csv_path.substr(slash + 1);

  std::ofstream csv(csv_path, std::ios::trunc);
  if (!csv) {
    std::cerr << "cannot write " << csv_path << '\n';
    return 2;
  }
  csv << timeseries_header() << std::flush;

  std::cout << "kcds-bots: " << scenario.name << ", " << bot_count << " bots on " << threads
            << " threads, " << scenario.duration_s << " s, run " << shared.run_tag << ", server "
            << facts.server_host << std::endl;
  std::signal(SIGINT, request_stop);
  std::signal(SIGTERM, request_stop);

  const double start = clock_seconds();
  const auto start_usage = metrics::sample_process_usage();
  std::thread worker([&] { runner.run(start, scenario.duration_s, stop_requested); });
  std::atomic<bool> finished{false};
  std::vector<ServerSample> samples;
  ThreadStatus previous;
  auto previous_usage = start_usage;
  double previous_at = start;
  const double report_every = options.json_status ? 1.0 : 5.0;
  double next_report = start + report_every;
  std::thread waiter([&] {
    worker.join();
    finished.store(true);
  });
  while (!finished.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const double now = clock_seconds();
    if (now < next_report && !finished.load()) {
      continue;
    }
    next_report = now + report_every;
    TimePoint point;
    point.elapsed_s = now - start;
    if (metrics_url) {
      if (const auto body = http_get(*metrics_url)) {
        point.server = read_server_sample(parse_prometheus(*body));
      }
      point.server.elapsed_s = point.elapsed_s;
      samples.push_back(point.server);
    }
    const ThreadStatus status = runner.status();
    const double seconds = std::max(0.001, now - previous_at);
    const auto usage = metrics::sample_process_usage();
    point.bots_playing = status.playing;
    point.bots_failed = status.failed;
    point.kbps_in =
        double(status.bytes_received - previous.bytes_received) * 8.0 / 1000.0 / seconds;
    point.kbps_out = double(status.bytes_sent - previous.bytes_sent) * 8.0 / 1000.0 / seconds;
    point.corrections = status.corrections;
    point.refused_joins = status.refused_joins;
    point.bot_cpu_percent = metrics::cpu_percent_of_one_core(previous_usage, usage);
    point.bot_overruns = status.overruns;
    csv << timeseries_row(point) << std::flush;
    if (options.json_status) {
      // For the npm CLI, which adds up the lines of all its engines.
      std::printf(
          "KCDS_BOTS_STATUS {\"elapsed_s\":%.2f,\"bots\":%zu,\"playing\":%zu,\"failed\":%zu,"
          "\"bytes_in\":%llu,\"bytes_out\":%llu,\"corrections\":%llu,\"refused_joins\":%llu,"
          "\"ticks\":%llu,\"overruns\":%llu,\"cpu_pct\":%.1f}\n",
          point.elapsed_s, status.bots, status.playing, status.failed,
          (unsigned long long)status.bytes_received, (unsigned long long)status.bytes_sent,
          (unsigned long long)status.corrections, (unsigned long long)status.refused_joins,
          (unsigned long long)status.ticks, (unsigned long long)status.overruns,
          point.bot_cpu_percent);
      std::fflush(stdout);
      previous = status;
      previous_usage = usage;
      previous_at = now;
      continue;
    }
    std::printf(
        "[%6.0fs] bots %zu/%zu playing, %zu failed | in %.0f kbps, out %.0f kbps | corrections "
        "%.1f/min | refused joins %llu | server %s tick p99 %.2f ms, refused checks %.0f\n",
        point.elapsed_s, status.playing, status.bots, status.failed, point.kbps_in, point.kbps_out,
        double(status.corrections - previous.corrections) * 60.0 / seconds,
        (unsigned long long)status.refused_joins, point.server.ok ? "ok" : "-",
        point.server.tick_p99_ms,
        point.server.movement_refusals_total + point.server.melee_refusals_total);
    std::fflush(stdout);
    previous = status;
    previous_usage = usage;
    previous_at = now;
  }
  waiter.join();

  const auto end_usage = metrics::sample_process_usage();
  const ThreadStatus status = runner.status();
  facts.duration_s = clock_seconds() - start;
  facts.interrupted = stop_requested.load();
  facts.ticks = status.ticks;
  facts.overruns = status.overruns;
  facts.bot_cpu_percent = metrics::cpu_percent_of_one_core(start_usage, end_usage);
  facts.bot_rss_mb = double(end_usage.peak_working_set_bytes) / 1048576.0;

  BotStats total;
  std::vector<BotNumbers> numbers;
  runner.each_bot([&](const Bot& bot) {
    total.merge(bot.stats());
    if (bot.stats().playing_seconds < 1.0) {
      return;
    }
    BotNumbers one;
    one.group = bot.group();
    const auto traffic = bot.traffic();
    const double seconds = bot.stats().playing_seconds;
    one.kbps_in = double(traffic.bytes_received) * 8.0 / 1000.0 / seconds;
    one.kbps_out = double(traffic.bytes_sent) * 8.0 / 1000.0 / seconds;
    one.corrections_per_min = double(bot.stats().corrections) * 60.0 / seconds;
    numbers.push_back(one);
  });
  std::ofstream out(options.out_path, std::ios::trunc);
  out << result_json(facts, total, numbers, samples) << '\n';
  if (!out) {
    std::cerr << "cannot write " << options.out_path << '\n';
    return 2;
  }
  std::cout << "kcds-bots: " << total.spawned << " spawned, "
            << total.refused_joins + total.timed_out_joins << " failed connects, "
            << total.corrections << " corrections; " << options.out_path
            << (facts.interrupted ? " (interrupted: partial)" : "") << std::endl;
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  BotsOptions options;
  try {
    const std::vector<std::string> arguments(argv + 1, argv + argc);
    if (arguments.size() == 1 && (arguments[0] == "--help" || arguments[0] == "-h")) {
      std::cout << bots_usage();
      return 0;
    }
    options = parse_bots_options(arguments);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n' << bots_usage();
    return 2;
  }
  try {
    return run(options);
  } catch (const ScenarioError& error) {
    std::cerr << options.scenario_path << ":\n" << error.what() << '\n';
    return 2;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
