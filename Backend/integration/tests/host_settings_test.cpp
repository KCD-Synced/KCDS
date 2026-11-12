// Scaling settings of the local KSTP host (phase 2): defaults, environment parsing and
// the derived per-tick snapshot budget.
#include "../local/config/host_settings.hpp"
#include "../local/config/levels.hpp"
#include "../local/config/server_json.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "kcd2s/testkit.hpp"

using kcd2s::local::config::HostSettings;
using kcd2s::local::config::read_host_settings;

namespace {

HostSettings read(const std::map<std::string, std::string>& environment,
                  std::vector<std::string>& problems, double tick_rate = 60.0,
                  unsigned hardware_threads = 8) {
  return read_host_settings(
      tick_rate,
      [&](const char* name) -> std::optional<std::string> {
        const auto found = environment.find(name);
        if (found == environment.end()) {
          return std::nullopt;
        }
        return found->second;
      },
      problems, hardware_threads);
}

}  // namespace

KCD2S_TEST(host_settings, defaults_keep_the_cap_and_bound_interest) {
  std::vector<std::string> problems;
  const HostSettings settings = read({}, problems);
  KCD2S_CHECK(problems.empty());
  KCD2S_CHECK_EQ(settings.max_peers, 32u);
  KCD2S_CHECK_EQ(settings.interest_radius, 150.0f);
  KCD2S_CHECK_EQ(settings.interest_hysteresis, 25.0f);
  KCD2S_CHECK_EQ(settings.max_datagram_bytes, std::size_t{1200});
  // A dedicated server uses its cores by default: eight hardware threads, seven workers.
  KCD2S_CHECK(settings.parallel_snapshots());
  KCD2S_CHECK_EQ(settings.snapshot_workers, 7u);
  KCD2S_CHECK_EQ(settings.receive_budget(), 1024u);
  KCD2S_CHECK_EQ(settings.interest_cell_size(), 75.0f);
  const kcd2s::integration::SnapshotBudget budget = settings.snapshot_budget();
  KCD2S_CHECK_EQ(budget.peer_bytes_per_tick, std::uint32_t{4369});     // 256 KiB/s at 60 Hz
  KCD2S_CHECK_EQ(budget.total_bytes_per_tick, std::uint64_t{838860});  // 48 MiB/s at 60 Hz
}

KCD2S_TEST(host_settings, environment_overrides_and_rejects_invalid_values) {
  std::vector<std::string> problems;
  const HostSettings settings = read({{"KCDS_SERVER_MAX_PEERS", "100"},
                                      {"KCDS_SERVER_INTEREST_RADIUS", "80.5"},
                                      {"KCDS_SERVER_INTEREST_HYSTERESIS", "0"},
                                      {"KCDS_SERVER_PEER_SNAPSHOT_BYTES_PER_S", "120000"},
                                      {"KCDS_SERVER_SNAPSHOT_BYTES_PER_S", "nonsense"},
                                      {"KCDS_SERVER_MAX_DATAGRAM", "500"},
                                      {"KCDS_SERVER_SNAPSHOT_WORKERS", "4"}},
                                     problems, 30.0);
  KCD2S_CHECK_EQ(settings.max_peers, 100u);
  KCD2S_CHECK_EQ(settings.interest_radius, 80.5f);
  KCD2S_CHECK_EQ(settings.interest_hysteresis, 0.0f);
  KCD2S_CHECK_EQ(settings.peer_snapshot_bytes_per_second, std::uint64_t{120000});
  KCD2S_CHECK_EQ(settings.total_snapshot_bytes_per_second,
                 kcd2s::local::config::kDefaultTotalSnapshotBytesPerSecond);
  KCD2S_CHECK_EQ(settings.max_datagram_bytes, std::size_t{1200});  // 500 is below the 576 floor
  KCD2S_CHECK(settings.parallel_snapshots());
  KCD2S_CHECK_EQ(settings.receive_budget(), 1024u);
  const kcd2s::integration::SnapshotBudget budget = settings.snapshot_budget();
  KCD2S_CHECK_EQ(budget.peer_bytes_per_tick, std::uint32_t{4000});
  KCD2S_CHECK_EQ(problems.size(), std::size_t{2});
}

KCD2S_TEST(host_settings, snapshot_workers_default_to_the_machine) {
  using kcd2s::local::config::default_snapshot_workers;
  KCD2S_CHECK_EQ(default_snapshot_workers(0), 0u);
  KCD2S_CHECK_EQ(default_snapshot_workers(1), 0u);
  KCD2S_CHECK_EQ(default_snapshot_workers(2), 2u);
  KCD2S_CHECK_EQ(default_snapshot_workers(4), 3u);
  KCD2S_CHECK_EQ(default_snapshot_workers(64), 16u);
  std::vector<std::string> problems;
  // A single-thread machine builds serially; 0 or 1 asks for a serial build anywhere.
  KCD2S_CHECK(!read({}, problems, 60.0, 1).parallel_snapshots());
  KCD2S_CHECK(!read({{"KCDS_SERVER_SNAPSHOT_WORKERS", "0"}}, problems).parallel_snapshots());
  KCD2S_CHECK(!read({{"KCDS_SERVER_SNAPSHOT_WORKERS", "1"}}, problems).parallel_snapshots());
  KCD2S_CHECK_EQ(read({{"KCDS_SERVER_SNAPSHOT_WORKERS", "32"}}, problems).snapshot_workers, 32u);
  KCD2S_CHECK(problems.empty());
}

KCD2S_TEST(host_settings, usable_cpus_follow_pinning_and_quota) {
  using kcd2s::local::config::cfs_quota;
  using kcd2s::local::config::cpu_max_quota;
  using kcd2s::local::config::usable_cpus;
  // cgroup v2 cpu.max: "max" is no quota; docker --cpus=3 writes "300000 100000".
  KCD2S_CHECK(!cpu_max_quota("max 100000"));
  KCD2S_CHECK(!cpu_max_quota(""));
  KCD2S_CHECK(!cpu_max_quota("lots 100000"));
  KCD2S_CHECK_EQ(cpu_max_quota("300000 100000").value_or(0), 3.0);
  KCD2S_CHECK_EQ(cpu_max_quota("150000 100000").value_or(0), 1.5);
  // cgroup v1: a quota of -1 is none.
  KCD2S_CHECK(!cfs_quota(-1, 100000));
  KCD2S_CHECK_EQ(cfs_quota(200000, 100000).value_or(0), 2.0);
  // 28 hardware threads under a 3-CPU quota: 3 usable, so 2 snapshot workers, not 16.
  KCD2S_CHECK_EQ(usable_cpus(28, std::nullopt, std::nullopt), 28u);
  KCD2S_CHECK_EQ(usable_cpus(28, std::nullopt, 3.0), 3u);
  KCD2S_CHECK_EQ(kcd2s::local::config::default_snapshot_workers(usable_cpus(28, {}, 3.0)), 2u);
  // A partial CPU still runs a thread; pinning counts too; the tightest limit wins.
  KCD2S_CHECK_EQ(usable_cpus(28, std::nullopt, 2.5), 3u);
  KCD2S_CHECK_EQ(usable_cpus(28, 4u, std::nullopt), 4u);
  KCD2S_CHECK_EQ(usable_cpus(28, 8u, 2.0), 2u);
  KCD2S_CHECK_EQ(usable_cpus(4, std::nullopt, 0.5), 1u);
  KCD2S_CHECK_EQ(usable_cpus(0, std::nullopt, std::nullopt), 1u);
}

KCD2S_TEST(host_settings, cgroup_quota_is_the_tightest_on_the_way_to_the_root) {
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / "kcds-cgroup-test";
  fs::remove_all(root);
  fs::create_directories(root / "system.slice" / "kcds.service");
  const auto write = [](const fs::path& file, const char* text) {
    std::ofstream(file) << text << "\n";
  };
  write(root / "system.slice" / "cpu.max", "200000 100000");
  write(root / "system.slice" / "kcds.service" / "cpu.max", "max 100000");
  const auto quota = [&](const char* group) {
    return kcd2s::local::config::cgroup_v2_quota(root.string(), group).value_or(0);
  };
  // The service has no quota of its own; its slice allows 2 CPUs.
  KCD2S_CHECK_EQ(quota("/system.slice/kcds.service"), 2.0);
  write(root / "system.slice" / "kcds.service" / "cpu.max", "300000 100000");
  KCD2S_CHECK_EQ(quota("/system.slice/kcds.service"), 2.0);
  write(root / "system.slice" / "kcds.service" / "cpu.max", "100000 100000");
  KCD2S_CHECK_EQ(quota("/system.slice/kcds.service"), 1.0);
  // Inside a container the process's group is the root of its own hierarchy.
  write(root / "cpu.max", "300000 100000");
  KCD2S_CHECK_EQ(quota("/"), 3.0);
  using kcd2s::local::config::cgroup_v2_quota;
  KCD2S_CHECK(!cgroup_v2_quota((root / "missing").string(), "/"));
  fs::remove_all(root);
}

KCD2S_TEST(host_settings, distance_rates_are_on_unless_switched_off) {
  std::vector<std::string> problems;
  KCD2S_CHECK(read({}, problems).distance_rates);
  KCD2S_CHECK(!read({{"KCDS_SERVER_DISTANCE_RATES", "off"}}, problems).distance_rates);
  KCD2S_CHECK(read({{"KCDS_SERVER_DISTANCE_RATES", "on"}}, problems).distance_rates);
  KCD2S_CHECK(problems.empty());
  KCD2S_CHECK(read({{"KCDS_SERVER_DISTANCE_RATES", "maybe"}}, problems).distance_rates);
  KCD2S_CHECK_EQ(problems.size(), std::size_t{1});
}

KCD2S_TEST(host_settings, bots_are_off_unless_asked_for) {
  std::vector<std::string> problems;
  KCD2S_CHECK(!read({}, problems).allow_bots);
  KCD2S_CHECK(read({{"KCDS_ALLOW_BOTS", "1"}}, problems).allow_bots);
  KCD2S_CHECK(!read({{"KCDS_ALLOW_BOTS", "0"}}, problems).allow_bots);
  KCD2S_CHECK(problems.empty());
  KCD2S_CHECK(!read({{"KCDS_ALLOW_BOTS", "yes please"}}, problems).allow_bots);
  KCD2S_CHECK_EQ(problems.size(), std::size_t{1});
}

// Bots are switched on by hand for a load test: server.json has no setting for them.
KCD2S_TEST(host_settings, server_json_cannot_allow_bots) {
  for (const auto& setting : kcd2s::local::config::kFileSettings) {
    KCD2S_CHECK(std::string(setting.variable) != "KCDS_ALLOW_BOTS");
  }
}

KCD2S_TEST(host_settings, movement_check_enforces_unless_set) {
  using kcd2s::integration::MovementCheckMode;
  std::vector<std::string> problems;
  KCD2S_CHECK(read({}, problems).movement_check == MovementCheckMode::Enforce);
  KCD2S_CHECK(read({{"KCDS_SERVER_MOVEMENT_CHECK", "observe"}}, problems).movement_check ==
              MovementCheckMode::Observe);
  KCD2S_CHECK(read({{"KCDS_SERVER_MOVEMENT_CHECK", "off"}}, problems).movement_check ==
              MovementCheckMode::Off);
  KCD2S_CHECK(problems.empty());
  read({{"KCDS_SERVER_MOVEMENT_CHECK", "strict"}}, problems);
  KCD2S_CHECK_EQ(problems.size(), std::size_t{1});
}

KCD2S_TEST(host_settings, melee_reach_check_enforces_unless_set) {
  using kcd2s::integration::MeleeReachMode;
  std::vector<std::string> problems;
  KCD2S_CHECK(read({}, problems).melee_reach_check == MeleeReachMode::Enforce);
  KCD2S_CHECK(read({{"KCDS_SERVER_MELEE_REACH_CHECK", "observe"}}, problems).melee_reach_check ==
              MeleeReachMode::Observe);
  KCD2S_CHECK(read({{"KCDS_SERVER_MELEE_REACH_CHECK", "off"}}, problems).melee_reach_check ==
              MeleeReachMode::Off);
  KCD2S_CHECK(problems.empty());
  read({{"KCDS_SERVER_MELEE_REACH_CHECK", "far"}}, problems);
  KCD2S_CHECK_EQ(problems.size(), std::size_t{1});
}

KCD2S_TEST(host_settings, peer_cap_has_a_hard_limit) {
  std::vector<std::string> problems;
  const HostSettings too_many = read({{"KCDS_SERVER_MAX_PEERS", "5000"}}, problems);
  KCD2S_CHECK_EQ(too_many.max_peers, 32u);
  const HostSettings fractional = read({{"KCDS_SERVER_MAX_PEERS", "64.5"}}, problems);
  KCD2S_CHECK_EQ(fractional.max_peers, 32u);
  const HostSettings large = read({{"KCDS_SERVER_MAX_PEERS", "1024"}}, problems);
  KCD2S_CHECK_EQ(large.max_peers, 1024u);
  KCD2S_CHECK_EQ(large.receive_budget(), 8192u);
  KCD2S_CHECK_EQ(problems.size(), std::size_t{2});
}

KCD2S_TEST(host_settings, character_budget_is_opt_in_and_validates_whole_numbers) {
  std::vector<std::string> problems;
  KCD2S_CHECK_EQ(read({}, problems).max_characters, 0u);
  KCD2S_CHECK_EQ(read({{"KCDS_SERVER_MAX_CHARACTERS", "80"}}, problems).max_characters, 80u);
  KCD2S_CHECK_EQ(read({{"KCDS_SERVER_MAX_CHARACTERS", "0"}}, problems).max_characters, 0u);
  KCD2S_CHECK_EQ(read({{"KCDS_SERVER_MAX_CHARACTERS", "4096"}}, problems).max_characters, 4096u);
  KCD2S_CHECK(problems.empty());
  for (const auto* invalid : {"-1", "1.5", "4097", "nan"}) {
    KCD2S_CHECK_EQ(read({{"KCDS_SERVER_MAX_CHARACTERS", invalid}}, problems).max_characters, 0u);
  }
  KCD2S_CHECK_EQ(problems.size(), std::size_t{4});
  problems.clear();
  const auto file = kcd2s::local::config::read_server_file(
      R"({"maxCharacters": 80})", [](const char*) { return false; }, problems);
  const std::map<std::string, std::string> variables(file.variables.begin(), file.variables.end());
  KCD2S_CHECK_EQ(variables.at("KCDS_SERVER_MAX_CHARACTERS"), std::string("80"));
  KCD2S_CHECK(problems.empty());
}

// The levels a server can run: the game's three worlds and World Architect's custom
// maps (kcds_map_<name>), nothing else.
KCD2S_TEST(host_settings, only_the_games_levels_are_run) {
  KCD2S_CHECK(kcd2s::local::config::known_level("trosecko"));
  KCD2S_CHECK(kcd2s::local::config::known_level("kutnohorsko"));
  KCD2S_CHECK(kcd2s::local::config::known_level("klaster"));
  KCD2S_CHECK(!kcd2s::local::config::known_level("Trosecko"));
  KCD2S_CHECK(!kcd2s::local::config::known_level(""));
  KCD2S_CHECK(!kcd2s::local::config::known_level("../trosecko"));
  KCD2S_CHECK(kcd2s::local::config::known_level("kcds_map_prague"));
  KCD2S_CHECK(!kcd2s::local::config::known_level("kcds_map_"));
  KCD2S_CHECK(!kcd2s::local::config::known_level("kcds_map_Prague"));
  KCD2S_CHECK(!kcd2s::local::config::known_level("kcds_map_../x"));
  KCD2S_CHECK_EQ(kcd2s::local::config::level_list(),
                 std::string("trosecko, kutnohorsko or klaster"));
}

// server.json and the command line: command line, environment, file, default.
KCD2S_TEST(host_settings, server_json_fills_what_the_environment_leaves) {
  namespace file = kcd2s::local::config;
  file::ServerStart start;
  std::string problem;
  KCD2S_CHECK(
      file::parse_command_line({"--port", "7800", "--level", "kutnohorsko"}, start, problem));
  KCD2S_CHECK(start.port == std::optional<std::string>("7800"));
  KCD2S_CHECK(start.level == std::optional<std::string>("kutnohorsko"));
  KCD2S_CHECK(!start.bind);
  file::ServerStart positional;
  KCD2S_CHECK(file::parse_command_line({"7777", "0.0.0.0", "trosecko", "node", "main.cjs", "res"},
                                       positional, problem));
  KCD2S_CHECK(positional.resources == std::optional<std::string>("res"));
  file::ServerStart bad;
  KCD2S_CHECK(!file::parse_command_line({"--colour", "red"}, bad, problem));
  KCD2S_CHECK(!file::parse_command_line({"--port"}, bad, problem));

  std::vector<std::string> problems;
  const auto read = file::read_server_file(
      R"({"port": 7790, "level": "klaster", "name": "Rattay", "maxPlayers": 64,
          "publicPlayerList": false, "restartTimes": ["06:00", "18:00"], "password": "pw",
          "tickRate": "fast", "colour": "red"})",
      [](const char* variable) { return std::string(variable) == "KCDS_SERVER_PASSWORD"; },
      problems);
  KCD2S_CHECK(read.start.port == std::optional<std::string>("7790"));
  KCD2S_CHECK(read.start.level == std::optional<std::string>("klaster"));
  std::map<std::string, std::string> variables(read.variables.begin(), read.variables.end());
  KCD2S_CHECK_EQ(variables["KCDS_SERVER_NAME"], std::string("Rattay"));
  KCD2S_CHECK_EQ(variables["KCDS_SERVER_MAX_PEERS"], std::string("64"));
  KCD2S_CHECK_EQ(variables["KCDS_PUBLIC_PLAYER_LIST"], std::string("off"));
  KCD2S_CHECK_EQ(variables["KCDS_RESTART_TIMES"], std::string("06:00,18:00"));
  // Set in the environment: the environment's stays.
  KCD2S_CHECK(variables.count("KCDS_SERVER_PASSWORD") == 0);
  // A wrong type and an unknown key: reported, left out.
  KCD2S_CHECK(variables.count("KCDS_SERVER_TICK_HZ") == 0);
  KCD2S_CHECK_EQ(problems.size(), std::size_t{2});
  problems.clear();
  KCD2S_CHECK(file::read_server_file(
                  "not json", [](const char*) { return false; }, problems)
                  .variables.empty());
  KCD2S_CHECK_EQ(problems.size(), std::size_t{1});
  KCD2S_CHECK_EQ(file::first_of(std::string("1"), std::string("2"), std::string("3"), "4"),
                 std::string("1"));
  KCD2S_CHECK_EQ(file::first_of(std::nullopt, std::string("2"), std::string("3"), "4"),
                 std::string("2"));
  KCD2S_CHECK_EQ(file::first_of(std::nullopt, std::nullopt, std::string("3"), "4"),
                 std::string("3"));
  KCD2S_CHECK_EQ(file::first_of(std::nullopt, std::string(""), std::nullopt, "4"),
                 std::string("4"));
}

KCD2S_TEST(host_settings, master_takes_true_false_and_the_old_on_off_or_an_address) {
  namespace file = kcd2s::local::config;
  const auto master_of = [](const std::string& json) {
    std::vector<std::string> problems;
    const auto read = file::read_server_file(json, [](const char*) { return false; }, problems);
    std::map<std::string, std::string> variables(read.variables.begin(), read.variables.end());
    return problems.empty() ? variables["KCDS_MASTER_URL"] : std::string("problem");
  };
  KCD2S_CHECK_EQ(master_of(R"({"master": true})"), std::string("on"));
  KCD2S_CHECK_EQ(master_of(R"({"master": false})"), std::string("off"));
  KCD2S_CHECK_EQ(master_of(R"({"master": "on"})"), std::string("on"));
  KCD2S_CHECK_EQ(master_of(R"({"master": "off"})"), std::string("off"));
  KCD2S_CHECK_EQ(master_of(R"({"master": "https://master.example.org"})"),
                 std::string("https://master.example.org"));
  KCD2S_CHECK_EQ(master_of(R"({"master": 1})"), std::string("problem"));
}

KCD2S_TEST(host_settings, voice_on_a_separate_server_needs_all_it_names) {
  std::map<std::string, std::string> values;
  const kcd2s::local::config::SettingLookup lookup =
      [&values](const char* name) -> std::optional<std::string> {
    const auto found = values.find(name);
    if (found == values.end()) {
      return std::nullopt;
    }
    return found->second;
  };
  std::vector<std::string> problems;
  KCD2S_CHECK(!read_host_settings(60.0, lookup, problems).voice_external);
  KCD2S_CHECK(problems.empty());

  values["KCDS_SERVER_VOICE_MODE"] = "external";
  values["KCDS_SERVER_VOICE_URL"] = "voice.example.com:7778";
  KCD2S_CHECK(!read_host_settings(60.0, lookup, problems).voice_external);
  KCD2S_CHECK_EQ(problems.size(), std::size_t{1});
  problems.clear();

  values["KCDS_SERVER_VOICE_ID"] = "main";
  values["KCDS_SERVER_VOICE_SECRET"] = "secret";
  const HostSettings settings = read_host_settings(60.0, lookup, problems);
  KCD2S_CHECK(settings.voice_external);
  KCD2S_CHECK(settings.voice_url == "voice.example.com:7778");
  KCD2S_CHECK(problems.empty());

  values["KCDS_SERVER_VOICE_MODE"] = "somewhere";
  KCD2S_CHECK(!read_host_settings(60.0, lookup, problems).voice_external);
  KCD2S_CHECK_EQ(problems.size(), std::size_t{1});
}
