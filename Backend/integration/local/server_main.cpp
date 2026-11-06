#include "runtime.hpp"
#include "config/levels.hpp"
#include "config/server_json.hpp"
#include "kcd2s/wire/packet.hpp"
#include "master/master_registration.hpp"
#include "metrics/server_metrics.hpp"
#include "tick/fixed_rate_schedule.hpp"
#ifdef _WIN32
#include "console_win.hpp"
#include "tick/tick_timer_win.hpp"
#else
#include "console_posix.hpp"
#include "tick/tick_timer_posix.hpp"
#endif
#include <atomic>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <sstream>
#include <memory>
#include <csignal>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>
namespace {
volatile std::sig_atomic_t running = 1;
// How long a stopping server keeps ticking so its players hear that it stops, in seconds.
constexpr double kStopSendSeconds = 0.3;
// The exit code that asks the start script (start-server.sh) to start the server again.
constexpr int kRestartExitCode = 75;
void stop(int) {
  running = 0;
}
std::optional<std::string> environment_value(const char* name) {
#ifdef _WIN32
  char* value = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&value, &length, name) != 0 || !value) {
    return std::nullopt;
  }
  std::string text(value);
  std::free(value);
  return text;
#else
  const char* value = std::getenv(name);
  if (!value) {
    return std::nullopt;
  }
  return std::string(value);
#endif
}
// Sets an environment variable of this process, which the resource host inherits too.
void set_environment(const std::string& name, const std::string& value) {
#ifdef _WIN32
  _putenv_s(name.c_str(), value.c_str());
#else
  setenv(name.c_str(), value.c_str(), 1);
#endif
}
constexpr const char* kUsage =
    "usage: kcd2s-server [--port N] [--bind ADDRESS] [--level LEVEL] [--node NODE] "
    "[--resource-host MAIN.CJS] [--resources FOLDER] [--config SERVER.JSON]\n"
    "   or: kcd2s-server PORT BIND LEVEL [NODE MAIN.CJS RESOURCES]";
// KCDS_SERVER_TICK_HZ selects the fixed tick rate (10-240 Hz, default 60 Hz).
double tick_rate_from_environment() {
  const auto value = environment_value("KCDS_SERVER_TICK_HZ");
  if (!value) {
    return kcd2s::local::tick::kDefaultTickRateHz;
  }
  const std::string text = *value;
  const auto parsed = kcd2s::local::tick::parse_tick_rate(text.c_str());
  if (!parsed) {
    std::cerr << "Ignoring invalid KCDS_SERVER_TICK_HZ=" << text << std::endl;
    return kcd2s::local::tick::kDefaultTickRateHz;
  }
  return *parsed;
}
}  // namespace
int main(int argc, char** argv) {
  try {
    namespace settings_file = kcd2s::local::config;
    // The command line, then the environment, then server.json, then the defaults.
    settings_file::ServerStart start;
    std::string argument_problem;
    if (!settings_file::parse_command_line(std::vector<std::string>(argv + 1, argv + argc), start,
                                           argument_problem)) {
      std::cerr << argument_problem << '\n' << kUsage << std::endl;
      return 2;
    }
    settings_file::ServerStart from_file;
    const std::string config_path = start.config.value_or("server.json");
    // KCDS-Admin edits this file: the resource host learns where it is.
    std::error_code absolute_error;
    const auto absolute_config = std::filesystem::absolute(config_path, absolute_error);
    if (!absolute_error) {
      set_environment("KCDS_SERVER_CONFIG", absolute_config.string());
    }
    // Read and closed at once: KCDS-Admin replaces the file while the server runs.
    std::optional<std::string> config_text;
    {
      std::ifstream config_file(config_path, std::ios::binary);
      if (config_file) {
        std::stringstream text;
        text << config_file.rdbuf();
        config_text = text.str();
      }
    }
    if (config_text) {
      std::vector<std::string> file_problems;
      const auto file = settings_file::read_server_file(
          *config_text, [](const char* name) { return environment_value(name).has_value(); },
          file_problems);
      for (const auto& problem : file_problems)
        std::cerr << problem << std::endl;
      for (const auto& [name, value] : file.variables)
        set_environment(name, value);
      from_file = file.start;
      std::cout << "CONFIG " << config_path << ": " << file.variables.size()
                << " settings the environment does not set" << std::endl;
    } else if (start.config) {
      std::cerr << "The settings file " << config_path << " could not be read" << std::endl;
      return 2;
    }
    const std::string port_text = settings_file::first_of(
        start.port, environment_value("KCDS_SERVER_PORT"), from_file.port, "7777");
    unsigned long port = 0;
    if (port_text.empty() || port_text.size() > 5 ||
        port_text.find_first_not_of("0123456789") != std::string::npos ||
        (port = std::stoul(port_text)) == 0 || port > 65535) {
      std::cerr << "Invalid port \"" << port_text << "\" (1 to 65535)" << std::endl;
      return 2;
    }
    const std::string ip = settings_file::first_of(
        start.bind, environment_value("KCDS_SERVER_BIND"), from_file.bind, "127.0.0.1");
    kcd2s::local::Winsock winsock;
    kcd2s::integration::WorldConfig config;
    config.map = settings_file::first_of(start.level, environment_value("KCDS_SERVER_LEVEL"),
                                         from_file.level, config.map);
    // A level the game does not have would load for nobody.
    if (!kcd2s::local::config::known_level(config.map)) {
      std::cerr << "Unknown level \"" << config.map << "\": the server runs "
                << kcd2s::local::config::level_list() << std::endl;
      return 2;
    }
    // The map lifted and every place discovered in the players' games, unless revealMap is off.
    config.reveal_map = environment_value("KCDS_SERVER_REVEAL_MAP").value_or("on") != "off";
    // The level's own people in every game, each run by one player's game (off by default).
    config.level_people = environment_value("KCDS_SERVER_LEVEL_PEOPLE").value_or("off") == "on";
    const double tick_rate = tick_rate_from_environment();
    std::vector<std::string> problems;
    const kcd2s::local::config::HostSettings settings =
        kcd2s::local::config::read_host_settings(tick_rate, environment_value, problems);
    for (const auto& problem : problems)
      std::cerr << problem << std::endl;
    kcd2s::local::Host host(ip.c_str(), static_cast<unsigned short>(port), config, settings);
    host.start_receive_thread();
    std::cout << "WORLD map=" << config.map
              << " debug=true quests=false revealMap=" << (config.reveal_map ? "on" : "off")
              << " levelPeople=" << (config.level_people ? "on" : "off") << " revision=1"
              << std::endl;
    // The resource host: Node.js, its main.cjs and the resources folder; none without all three.
    const std::string node =
        settings_file::first_of(start.node, environment_value("KCDS_NODE"), from_file.node, "");
    const std::string resource_host =
        settings_file::first_of(start.resource_host, std::nullopt, from_file.resource_host, "");
    const std::string resources = settings_file::first_of(
        start.resources, environment_value("KCDS_RESOURCES"), from_file.resources, "");
    if (!node.empty() && !resource_host.empty() && !resources.empty()) {
      using kcd2s::local::process_text;
      // The resource host serves the client files over HTTP on TCP with the game's own port and
      // address (web/file_server.cjs).
      set_environment("KCDS_GAME_PORT", port_text);
      set_environment("KCDS_GAME_BIND", ip);
      host.start_resources(process_text(node.c_str()), process_text(resource_host.c_str()),
                           process_text(resources.c_str()));
    }
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
#ifndef _WIN32
    // A resource host that died must not take the server down through a write to its pipe.
    std::signal(SIGPIPE, SIG_IGN);
#endif
    std::cout << "KCD2-Synced UDP " << ip << ':' << port << " (movement schema v1)" << std::endl;
    std::cout << "SCALING max_peers=" << settings.max_peers
              << " interest_radius=" << settings.interest_radius
              << " interest_hysteresis=" << settings.interest_hysteresis
              << " max_characters=" << settings.max_characters
              << " peer_snapshot_bytes_per_s=" << settings.peer_snapshot_bytes_per_second
              << " total_snapshot_bytes_per_s=" << settings.total_snapshot_bytes_per_second
              << " max_datagram=" << settings.max_datagram_bytes << " snapshot_workers="
              << (settings.parallel_snapshots() ? settings.snapshot_workers : 0) << std::endl;
    // Fixed-rate loop: ticks start at absolute deadlines; missed slots are skipped and counted.
    kcd2s::local::tick::FixedRateSchedule schedule(tick_rate);
    kcd2s::local::tick::TickTimer timer;
#ifdef _WIN32
    const std::string wait = timer.high_resolution()
                                 ? "high-resolution waitable timer"
                                 : "standard waitable timer (high resolution unavailable)";
#else
    const std::string wait = timer.description();
#endif
    const std::string scheduler = "fixed-rate, skips missed slots; " + wait;
    std::cout << "TICK rate_hz=" << tick_rate << " scheduler=" << scheduler << std::endl;
    // Opt-in metrics (KCDS_SERVER_METRICS=<interval seconds>); default off, no extra work.
    std::optional<kcd2s::local::metrics::ServerMetricsReporter> metrics;
    if (const auto interval = kcd2s::local::metrics::metrics_interval_from_environment()) {
      metrics.emplace(*interval, tick_rate, scheduler);
      metrics->print_configuration(config.map, settings, host);
      metrics->start(kcd2s::local::clock_seconds(), host);
    }
    // Public server browser listing (KCDS_MASTER_URL, KCDS_SERVER_NAME,
    // KCDS_SERVER_PUBLIC_ADDRESS); see master/registration_settings.hpp.
    std::vector<std::string> registration_problems;
    const auto registration = kcd2s::local::master::read_registration_settings(
        environment_value, ip, host.port(), registration_problems);
    host.set_server_name(kcd2s::local::master::server_name(environment_value));
    // Admission always needs the licence verifier, listed or not, localhost included.
    host.require_licence_verifier(true);
    for (const auto& problem : registration_problems)
      std::cerr << problem << std::endl;
    std::unique_ptr<kcd2s::local::master::MasterRegistration> listing;
    if (registration.enabled) {
      host.require_master_listing();
      listing = std::make_unique<kcd2s::local::master::MasterRegistration>(
          registration, config.map, int(settings.max_peers), int(kcd2s::wire::kProtocolVersion),
          KCDS_SERVER_VERSION);
    } else {
      std::cout << "MASTER private, not listed (" << registration.reason << ")" << std::endl;
    }
    kcd2s::local::ConsoleReader console;
    // What the players are told when the server stops, and the exit code: kRestartExitCode asks
    // the start script to start the server again.
    std::string stop_reason = "The server was shut down.";
    int exit_code = 0;
    // The listing id the scripts were last told (licence passes name it).
    std::string told_listing;
    schedule.start(kcd2s::local::clock_seconds());
    while (running) {
      for (const auto& command : console.take()) {
        if (command == "stop")
          running = 0;
        else
          host.console_command(command);
      }
      double tick_start = kcd2s::local::clock_seconds();
      while (tick_start < schedule.next_deadline()) {
        timer.wait(schedule.next_deadline() - tick_start);
        tick_start = kcd2s::local::clock_seconds();
      }
      const kcd2s::local::tick::TickSlot slot = schedule.begin_tick(tick_start);
      host.tick(tick_start);
      if (listing) {
        listing->set_players(int(host.member_count()));
        // The scripts check licence passes against the id the Master lists this server under.
        const std::string listing_id = listing->listing_id();
        if (listing_id != told_listing && host.master_listing(listing_id)) {
          told_listing = listing_id;
        }
      }
      if (metrics)
        metrics->record_tick(tick_start, kcd2s::local::clock_seconds(), slot, host);
      // The resource host stops the server (KCDS-Admin, a scheduled restart).
      if (auto stop = host.take_stop_request()) {
        if (!stop->reason.empty()) {
          stop_reason = stop->reason;
        }
        exit_code = stop->restart ? kRestartExitCode : 0;
        std::cout << "STOP requested by the resource host" << (stop->restart ? " (restart)" : "")
                  << std::endl;
        running = 0;
      }
    }
    // Stopping (stop, Ctrl+C, a service stop): the players' clients hear it and go back to their
    // main menu at once (SESSION_END); a few more ticks send it.
    std::cout << "STOP telling " << host.member_count() << " players" << std::endl;
    host.end_sessions(stop_reason);
    const double stop_until = kcd2s::local::clock_seconds() + kStopSendSeconds;
    while (kcd2s::local::clock_seconds() < stop_until) {
      host.tick(kcd2s::local::clock_seconds());
      timer.wait(1.0 / tick_rate);
    }
    return exit_code;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
