// kcds-bots scenario files (local/bots/scenario/scenario.hpp): valid files, every problem of an
// invalid one with its line, and the unit forms.
#include "../local/bots/scenario/scenario.hpp"

#include <cmath>
#include <optional>
#include <string>

#include "kcd2s/testkit.hpp"

using kcd2s::local::bots::parse_fraction;
using kcd2s::local::bots::parse_seconds;
using kcd2s::local::bots::Scenario;
using kcd2s::local::bots::ScenarioError;
using kcd2s::local::bots::ScenarioReader;

namespace {

const char* const kBattle =
    "name: battle\n"
    "duration: 10m\n"
    "seed: 42\n"
    "join_window: 30s\n"
    "network:\n"
    "  latency: [40ms, 120ms]\n"
    "  jitter: 15ms\n"
    "  loss: 1%\n"
    "areas:\n"
    "  field_west: { offset: [-60, 0], radius: 20 }\n"
    "  hill: { at: [512.5, 1730, 41], radius: 10 }\n"
    "groups:\n"
    "  - name: red\n"
    "    count: 50\n"
    "    spawn: { area: field_west, radius: 15 }\n"
    "    behavior: melee_fighter\n"
    "    params: { target_group: blue, block_chance: 0.4 }\n"
    "  - name: blue\n"
    "    count: 50\n"
    "    behavior: melee_fighter\n"
    "    network: { loss: 5% }\n"
    "  - name: archers\n"
    "    count: 20\n"
    "    spawn: { area: hill }\n"
    "    behavior: archer\n";

std::vector<std::string> problems_of(const std::string& text) {
  try {
    ScenarioReader().read(text);
  } catch (const ScenarioError& error) {
    return error.problems();
  }
  return {};
}

bool mentions(const std::vector<std::string>& problems, const std::string& part) {
  for (const auto& problem : problems) {
    if (problem.find(part) != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

KCD2S_TEST(bots_scenario, reads_a_battle) {
  const Scenario scenario = ScenarioReader().read(kBattle);
  KCD2S_CHECK_EQ(scenario.name, std::string("battle"));
  KCD2S_CHECK_EQ(scenario.duration_s, 600.0);
  KCD2S_CHECK_EQ(scenario.seed, std::uint64_t{42});
  KCD2S_CHECK_EQ(scenario.join_window_s, 30.0);
  KCD2S_CHECK(std::fabs(scenario.network.latency_ms.min - 40.0) < 1e-9);
  KCD2S_CHECK(std::fabs(scenario.network.latency_ms.max - 120.0) < 1e-9);
  KCD2S_CHECK(std::fabs(scenario.network.jitter_ms - 15.0) < 1e-9);
  KCD2S_CHECK(std::fabs(scenario.network.loss - 0.01) < 1e-12);
  KCD2S_CHECK_EQ(scenario.bot_count(), 120u);
  const auto& west = scenario.areas.at("field_west");
  KCD2S_CHECK(!west.absolute && west.x == -60.0 && west.radius == 20.0);
  KCD2S_CHECK(scenario.areas.at("hill").absolute);
  const auto* red = scenario.group("red");
  KCD2S_CHECK(red && red->area == "field_west" && red->spawn_radius == 15.0);
  KCD2S_CHECK_EQ(red->params.find("target_group")->scalar, std::string("blue"));
  KCD2S_CHECK(!red->network.has_value());
  const auto* blue = scenario.group("blue");
  KCD2S_CHECK(blue->network && std::fabs(blue->network->loss - 0.05) < 1e-12);
  KCD2S_CHECK(blue->area.empty());
}

KCD2S_TEST(bots_scenario, reports_every_problem_with_its_line) {
  const auto problems = problems_of(
      "duration: soon\n"
      "colour: red\n"
      "network: { loss: 150% }\n"
      "groups:\n"
      "  - name: a\n"
      "    count: 0\n"
      "    behavior: dance\n"
      "  - name: a\n"
      "    count: 2\n"
      "    behavior: idle\n"
      "    spawn: { area: nowhere }\n");
  KCD2S_CHECK(mentions(problems, "line 1: duration must be a time"));
  KCD2S_CHECK(mentions(problems, "line 2: unknown setting colour"));
  KCD2S_CHECK(mentions(problems, "line 3: loss must be a percentage"));
  KCD2S_CHECK(mentions(problems, "line 6: count must be a whole number"));
  KCD2S_CHECK(mentions(problems, "behavior must be one of idle, wander"));
  KCD2S_CHECK(mentions(problems, "group name given twice: a"));
  KCD2S_CHECK(mentions(problems, "unknown area nowhere"));
  KCD2S_CHECK(mentions(problems, "name is missing"));
}

KCD2S_TEST(bots_scenario, an_area_needs_one_position) {
  const std::string head = "name: x\ngroups: [{ name: g, count: 1, behavior: idle }]\nareas:\n";
  KCD2S_CHECK(mentions(problems_of(head + "  a: { radius: 3 }\n"), "needs offset"));
  KCD2S_CHECK(
      mentions(problems_of(head + "  a: { offset: [1, 2], at: [1, 2, 3] }\n"), "needs offset"));
  KCD2S_CHECK(mentions(problems_of(head + "  a: { offset: [1] }\n"), "offset is [x, y]"));
  KCD2S_CHECK(mentions(problems_of(head + "  a: { offset: [1, b] }\n"), "not a number"));
  KCD2S_CHECK(problems_of(head + "  a: { offset: [1, 2] }\n").empty());
}

KCD2S_TEST(bots_scenario, a_yaml_error_is_a_problem_too) {
  const auto problems = problems_of("name: [x\n");
  KCD2S_CHECK_EQ(problems.size(), std::size_t{1});
  KCD2S_CHECK(mentions(problems, "line 1"));
}

KCD2S_TEST(bots_scenario, areas_from_a_file_next_to_it) {
  ScenarioReader reader([](const std::string& name) -> std::optional<std::string> {
    if (name == "areas.yaml") {
      return std::string("areas:\n  square: { offset: [5, 5], radius: 30 }\n");
    }
    return std::nullopt;
  });
  const Scenario scenario = reader.read(
      "name: town\nareas_file: areas.yaml\n"
      "groups: [{ name: g, count: 3, behavior: wander, spawn: { area: square } }]\n");
  KCD2S_CHECK_EQ(scenario.areas.at("square").radius, 30.0);
  KCD2S_CHECK(mentions(problems_of("name: t\nareas_file: missing.yaml\n"
                                   "groups: [{ name: g, count: 1, behavior: idle }]\n"),
                       "cannot be read"));
}

KCD2S_TEST(bots_scenario, units) {
  KCD2S_CHECK_EQ(*parse_seconds("90s"), 90.0);
  KCD2S_CHECK_EQ(*parse_seconds("10m"), 600.0);
  KCD2S_CHECK_EQ(*parse_seconds("4h"), 14400.0);
  KCD2S_CHECK(std::fabs(*parse_seconds("250ms") - 0.25) < 1e-12);
  KCD2S_CHECK(std::fabs(*parse_seconds("0.8s") - 0.8) < 1e-12);
  KCD2S_CHECK_EQ(*parse_seconds("12"), 12.0);
  KCD2S_CHECK(!parse_seconds("-1s"));
  KCD2S_CHECK(!parse_seconds("ten"));
  KCD2S_CHECK(!parse_seconds(""));
  KCD2S_CHECK(std::fabs(*parse_fraction("1%") - 0.01) < 1e-12);
  KCD2S_CHECK_EQ(*parse_fraction("0.25"), 0.25);
  KCD2S_CHECK(!parse_fraction("2"));
  KCD2S_CHECK(!parse_fraction("101%"));
}
