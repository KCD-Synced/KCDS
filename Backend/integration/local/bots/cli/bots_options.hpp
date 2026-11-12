#pragma once
// kcds-bots' command line.
//
//   kcds-bots --server <host:port> --scenario <file.yaml> --out <result.json>
//             [--bots N] [--threads N] [--seed N] [--duration 10m]
//             [--metrics-url http://<host>:<port>/metrics]
//             [--share I/N] [--machines N] [--server-hardware "text"] [--tick-hz 60]
//
// --bots scales every group to N bots in all; --share I/N runs this machine's part of the
// scenario (every N-th bot of each group, from the I-th, 1-based), so N machines together run it
// once; --machines tells the result how many machines ran bots.
#include <cmath>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../scenario/scenario.hpp"

namespace kcd2s::local::bots {

struct BotsOptions {
  std::string host;
  unsigned short port = 0;
  std::string scenario_path;
  std::string out_path;
  std::optional<unsigned> bots;
  std::optional<unsigned> threads;
  std::optional<std::uint64_t> seed;
  std::optional<double> duration_s;
  std::string metrics_url;
  unsigned share_index = 1;  // 1-based
  unsigned share_count = 1;
  unsigned machines = 1;
  std::string server_hardware;
  double tick_hz = 60.0;
};

inline std::string bots_usage() {
  return "kcds-bots --server <host:port> --scenario <file.yaml> --out <result.json>\n"
         "          [--bots N] [--threads N] [--seed N] [--duration 10m]\n"
         "          [--metrics-url http://<host>:<port>/metrics]\n"
         "          [--share I/N] [--machines N] [--server-hardware \"text\"] [--tick-hz 60]\n"
         "The server must run with KCDS_ALLOW_BOTS=1 (a development build) and, for its\n"
         "numbers, KCDS_SERVER_METRICS=5 and KCDS_HTTP_PORT (where /metrics is served).\n";
}

inline BotsOptions parse_bots_options(const std::vector<std::string>& arguments) {
  BotsOptions options;
  auto value_of = [&](std::size_t& index) -> const std::string& {
    if (index + 1 >= arguments.size()) {
      throw std::invalid_argument(arguments[index] + " needs a value");
    }
    return arguments[++index];
  };
  auto whole = [](const std::string& name, const std::string& text, unsigned long low,
                  unsigned long high) {
    char* end = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &end, 10);
    if (text.empty() || *end != '\0' || value < low || value > high) {
      throw std::invalid_argument(name + " must be a whole number from " + std::to_string(low) +
                                  " to " + std::to_string(high));
    }
    return value;
  };
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const std::string& name = arguments[index];
    if (name == "--server") {
      const std::string& text = value_of(index);
      const std::size_t colon = text.rfind(':');
      if (colon == std::string::npos || colon == 0) {
        throw std::invalid_argument("--server must be host:port");
      }
      options.host = text.substr(0, colon);
      options.port =
          static_cast<unsigned short>(whole("--server port", text.substr(colon + 1), 1, 65535));
    } else if (name == "--scenario") {
      options.scenario_path = value_of(index);
    } else if (name == "--out") {
      options.out_path = value_of(index);
    } else if (name == "--bots") {
      options.bots = unsigned(whole(name, value_of(index), 1, 20000));
    } else if (name == "--threads") {
      options.threads = unsigned(whole(name, value_of(index), 1, 256));
    } else if (name == "--seed") {
      options.seed = whole(name, value_of(index), 0, 0xFFFFFFFFul);
    } else if (name == "--duration") {
      const auto seconds = parse_seconds(value_of(index));
      if (!seconds || *seconds <= 0.0) {
        throw std::invalid_argument("--duration must be a time such as 90s, 10m or 4h");
      }
      options.duration_s = *seconds;
    } else if (name == "--metrics-url") {
      options.metrics_url = value_of(index);
    } else if (name == "--share") {
      const std::string& text = value_of(index);
      const std::size_t slash = text.find('/');
      if (slash == std::string::npos) {
        throw std::invalid_argument("--share must be I/N, such as 1/2");
      }
      options.share_count = unsigned(whole("--share N", text.substr(slash + 1), 1, 64));
      options.share_index =
          unsigned(whole("--share I", text.substr(0, slash), 1, options.share_count));
    } else if (name == "--machines") {
      options.machines = unsigned(whole(name, value_of(index), 1, 64));
    } else if (name == "--server-hardware") {
      options.server_hardware = value_of(index);
    } else if (name == "--tick-hz") {
      options.tick_hz = double(whole(name, value_of(index), 10, 240));
    } else {
      throw std::invalid_argument("unknown option " + name);
    }
  }
  if (options.host.empty()) {
    throw std::invalid_argument("--server is required");
  }
  if (options.scenario_path.empty()) {
    throw std::invalid_argument("--scenario is required");
  }
  if (options.out_path.empty()) {
    throw std::invalid_argument("--out is required");
  }
  if (options.machines < options.share_count) {
    options.machines = options.share_count;
  }
  return options;
}

// Scales the groups so they add up to `bots`, keeping their proportions: each gets its share
// rounded down (at least one), and the largest group takes what is left over (or gives back what is
// too much), so the total is exact whenever there are at least as many bots as groups.
inline void scale_groups(Scenario& scenario, unsigned bots) {
  const unsigned total = scenario.bot_count();
  if (total == 0 || bots == total || scenario.groups.empty()) {
    return;
  }
  std::size_t largest = 0;
  for (std::size_t index = 1; index < scenario.groups.size(); ++index) {
    if (scenario.groups[index].count > scenario.groups[largest].count) {
      largest = index;
    }
  }
  long given = 0;
  for (Group& group : scenario.groups) {
    const auto share = unsigned(std::floor(double(group.count) * double(bots) / double(total)));
    group.count = std::max(1u, share);
    given += long(group.count);
  }
  const long left = long(bots) - given;
  Group& biggest = scenario.groups[largest];
  biggest.count = unsigned(std::max(1L, long(biggest.count) + left));
}

// The threads for `bots` bots: about 50 a thread, at most the machine's hardware threads.
inline unsigned default_threads(std::size_t bots) {
  const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
  const unsigned wanted = unsigned((bots + 49) / 50);
  return std::max(1u, std::min(wanted, hardware));
}

}  // namespace kcd2s::local::bots
