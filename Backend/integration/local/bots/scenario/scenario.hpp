#pragma once
// A kcds-bots scenario: who joins, where they go, what they do and over what network, read from a
// scenario file (scenarios/*.yaml, the YAML subset of yaml_subset.hpp). Every problem of a file is
// collected with its line, so a scenario author sees them all at once.
//
//   name: battle
//   duration: 10m          # 90s, 10m, 4h; a plain number is seconds
//   seed: 42
//   join_window: 30s       # bots connect spread over this time (0: all at once)
//   network: { latency: [40ms, 120ms], jitter: 15ms, loss: 1% }
//   areas:                 # or areas_file: areas.yaml (next to the scenario)
//     field_west: { offset: [-60, 0], radius: 20 }   # metres from where the server spawned a bot
//     hill: { at: [512.0, 1730.5, 41.0], radius: 10 }  # or a fixed world position
//   groups:
//     - name: red
//       count: 50
//       spawn: { area: field_west, radius: 15 }
//       behavior: melee_fighter
//       params: { target_group: blue }
//       network: { loss: 5% }  # this group's own conditions (optional)
//
// Areas are hand-written: no position comes from the game's files. A bot walks to its area at a
// speed the server's movement check allows, so no step of it is refused.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <locale>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "yaml_subset.hpp"

namespace kcd2s::local::bots {

struct Range {
  double min = 0.0;
  double max = 0.0;
};

// Conditions a bot's own network adds to every datagram, both ways.
struct NetworkConditions {
  Range latency_ms;        // one-way delay, picked per bot within the range
  double jitter_ms = 0.0;  // up to this much more per datagram
  double loss = 0.0;       // fraction of datagrams dropped, 0..1
};

struct Area {
  std::string name;
  // Metres from the bot's spawn (x east, y north), or a world position when `absolute`.
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  bool absolute = false;
  double radius = 10.0;
};

struct Group {
  std::string name;
  unsigned count = 0;
  std::string area;         // where its bots go ("" for where the server spawned them)
  double spawn_radius = 0;  // spread around the area's centre, 0 for the area's radius
  std::string behavior;
  yaml::Node params;  // the behaviour's own settings (a map, or null)
  std::optional<NetworkConditions> network;
  int line = 0;
};

struct Scenario {
  std::string name;
  double duration_s = 60.0;
  std::uint64_t seed = 1;
  double join_window_s = 0.0;
  NetworkConditions network;
  std::map<std::string, Area> areas;
  std::vector<Group> groups;

  unsigned bot_count() const {
    unsigned total = 0;
    for (const Group& group : groups) {
      total += group.count;
    }
    return total;
  }
  const Group* group(const std::string& name) const {
    for (const Group& one : groups) {
      if (one.name == name) {
        return &one;
      }
    }
    return nullptr;
  }
};

// The behaviours kcds-bots knows (behaviours/).
inline const std::vector<std::string>& known_behaviors() {
  static const std::vector<std::string> names{"idle",  "wander", "melee_fighter", "archer",
                                              "rider", "voice",  "join_leave"};
  return names;
}

namespace scenario_detail {

inline std::optional<double> number(const std::string& text) {
  if (text.empty()) {
    return std::nullopt;
  }
  std::istringstream in(text);
  in.imbue(std::locale::classic());
  double value = 0.0;
  in >> value;
  if (in.fail() || !in.eof() || !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

}  // namespace scenario_detail

// "90s", "10m", "4h", "250ms" or a plain number of seconds; nullopt for anything else.
inline std::optional<double> parse_seconds(const std::string& text) {
  struct Unit {
    const char* suffix;
    double seconds;
  };
  for (const Unit unit : {Unit{"ms", 0.001}, Unit{"s", 1.0}, Unit{"m", 60.0}, Unit{"h", 3600.0}}) {
    const std::string suffix = unit.suffix;
    if (text.size() > suffix.size() &&
        text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0) {
      const auto value = scenario_detail::number(text.substr(0, text.size() - suffix.size()));
      if (!value || *value < 0.0) {
        return std::nullopt;
      }
      return *value * unit.seconds;
    }
  }
  const auto value = scenario_detail::number(text);
  if (!value || *value < 0.0) {
    return std::nullopt;
  }
  return value;
}

// "1%" or a plain fraction 0..1.
inline std::optional<double> parse_fraction(const std::string& text) {
  if (!text.empty() && text.back() == '%') {
    const auto value = scenario_detail::number(text.substr(0, text.size() - 1));
    if (!value || *value < 0.0 || *value > 100.0) {
      return std::nullopt;
    }
    return *value / 100.0;
  }
  const auto value = scenario_detail::number(text);
  if (!value || *value < 0.0 || *value > 1.0) {
    return std::nullopt;
  }
  return value;
}

inline std::optional<double> parse_number(const std::string& text) {
  return scenario_detail::number(text);
}

// What went wrong in a scenario file: every problem, each with its line.
class ScenarioError : public std::runtime_error {
 public:
  explicit ScenarioError(const std::vector<std::string>& problems)
      : std::runtime_error(joined(problems)), problems_(problems) {
  }
  const std::vector<std::string>& problems() const {
    return problems_;
  }

 private:
  static std::string joined(const std::vector<std::string>& problems) {
    std::string text;
    for (const std::string& problem : problems) {
      text += (text.empty() ? "" : "\n") + problem;
    }
    return text;
  }
  std::vector<std::string> problems_;
};

class ScenarioReader {
 public:
  // `read_file(name)`: the text of a file next to the scenario (areas_file), or nullopt.
  using FileReader = std::function<std::optional<std::string>(const std::string&)>;

  explicit ScenarioReader(FileReader read_file = {}) : read_file_(std::move(read_file)) {
  }

  Scenario read(const std::string& text) {
    yaml::Node root;
    try {
      root = yaml::parse(text);
    } catch (const yaml::ParseError& error) {
      throw ScenarioError({error.what()});
    }
    Scenario scenario;
    if (!root.is_map()) {
      throw ScenarioError({"line 1: a scenario is a map of settings"});
    }
    for (const auto& [key, value] : root.map) {
      if (key == "name") {
        scenario.name = scalar(value, key);
      } else if (key == "duration") {
        scenario.duration_s = seconds(value, key, 1.0);
      } else if (key == "seed") {
        const auto seed = parse_number(scalar(value, key));
        if (!seed || *seed < 0 || *seed != std::floor(*seed)) {
          problem(value, "seed must be a whole number");
        } else {
          scenario.seed = static_cast<std::uint64_t>(*seed);
        }
      } else if (key == "join_window") {
        scenario.join_window_s = seconds(value, key, 0.0);
      } else if (key == "network") {
        scenario.network = network(value);
      } else if (key == "areas") {
        read_areas(value, scenario);
      } else if (key == "areas_file") {
        read_areas_file(value, scenario);
      } else if (key == "groups") {
        read_groups(value, scenario);
      } else {
        problem(value, "unknown setting " + key);
      }
    }
    if (scenario.name.empty()) {
      problem(root, "name is missing");
    }
    if (scenario.groups.empty()) {
      problem(root, "groups is missing or empty");
    }
    check_groups(scenario);
    if (!problems_.empty()) {
      throw ScenarioError(problems_);
    }
    return scenario;
  }

 private:
  void problem(const yaml::Node& node, const std::string& what) {
    problems_.push_back("line " + std::to_string(node.line) + ": " + what);
  }

  std::string scalar(const yaml::Node& node, const std::string& key) {
    if (!node.is_scalar()) {
      problem(node, key + " must be a single value");
      return {};
    }
    return node.scalar;
  }

  double seconds(const yaml::Node& node, const std::string& key, double minimum) {
    const auto value = parse_seconds(scalar(node, key));
    if (!value || *value < minimum) {
      problem(node, key + " must be a time such as 90s, 10m or 4h" +
                        (minimum > 0 ? " (more than 0)" : ""));
      return minimum;
    }
    return *value;
  }

  Range range(const yaml::Node& node, const std::string& key) {
    if (node.is_scalar()) {
      const auto value = parse_seconds(node.scalar);
      if (!value) {
        problem(node, key + " must be a time or [min, max]");
        return {};
      }
      return {*value * 1000.0, *value * 1000.0};
    }
    if (!node.is_list() || node.list.size() != 2) {
      problem(node, key + " must be a time or [min, max]");
      return {};
    }
    const auto low = parse_seconds(scalar(node.list[0], key));
    const auto high = parse_seconds(scalar(node.list[1], key));
    if (!low || !high || *low > *high) {
      problem(node, key + " must be [min, max] times, min first");
      return {};
    }
    return {*low * 1000.0, *high * 1000.0};
  }

  NetworkConditions network(const yaml::Node& node) {
    NetworkConditions conditions;
    if (!node.is_map()) {
      problem(node, "network must be a map");
      return conditions;
    }
    for (const auto& [key, value] : node.map) {
      if (key == "latency") {
        conditions.latency_ms = range(value, key);
      } else if (key == "jitter") {
        conditions.jitter_ms = seconds(value, key, 0.0) * 1000.0;
      } else if (key == "loss") {
        const auto loss = parse_fraction(scalar(value, key));
        if (!loss) {
          problem(value, "loss must be a percentage such as 1% (or a fraction 0..1)");
        } else {
          conditions.loss = *loss;
        }
      } else {
        problem(value, "unknown network setting " + key);
      }
    }
    if (conditions.latency_ms.max > 5000.0 || conditions.jitter_ms > 2000.0) {
      problem(node, "latency and jitter must stay below 5 s and 2 s");
    }
    return conditions;
  }

  void read_areas(const yaml::Node& node, Scenario& scenario) {
    if (!node.is_map()) {
      problem(node, "areas must be a map of named areas");
      return;
    }
    for (const auto& [name, value] : node.map) {
      Area area;
      area.name = name;
      if (!value.is_map()) {
        problem(value, "area " + name + " must be a map ({ offset: [x, y], radius: r })");
        continue;
      }
      const yaml::Node* offset = value.find("offset");
      const yaml::Node* at = value.find("at");
      if ((offset != nullptr) == (at != nullptr)) {
        problem(value, "area " + name + " needs offset: [x, y] or at: [x, y, z], one of them");
        continue;
      }
      const yaml::Node& point = offset ? *offset : *at;
      const std::size_t needed = offset ? 2 : 3;
      if (!point.is_list() || point.list.size() != needed) {
        problem(point, "area " + name + (offset ? " offset is [x, y]" : " at is [x, y, z]"));
        continue;
      }
      double values[3] = {};
      bool ok = true;
      for (std::size_t i = 0; i < needed; ++i) {
        const auto value_i = parse_number(scalar(point.list[i], "a coordinate"));
        ok = ok && value_i.has_value();
        values[i] = value_i.value_or(0.0);
      }
      if (!ok) {
        problem(point, "area " + name + " has a coordinate that is not a number");
        continue;
      }
      area.x = values[0];
      area.y = values[1];
      area.z = values[2];
      area.absolute = at != nullptr;
      if (const yaml::Node* radius = value.find("radius")) {
        const auto r = parse_number(scalar(*radius, "radius"));
        if (!r || *r <= 0.0 || *r > 2000.0) {
          problem(*radius, "area " + name + " radius must be a number of metres, 0..2000");
        } else {
          area.radius = *r;
        }
      }
      for (const auto& [setting, setting_value] : value.map) {
        if (setting != "offset" && setting != "at" && setting != "radius") {
          problem(setting_value, "unknown area setting " + setting);
        }
      }
      scenario.areas[name] = area;
    }
  }

  void read_areas_file(const yaml::Node& node, Scenario& scenario) {
    const std::string name = scalar(node, "areas_file");
    if (name.empty()) {
      return;
    }
    const auto text = read_file_ ? read_file_(name) : std::nullopt;
    if (!text) {
      problem(node, "areas_file " + name + " cannot be read");
      return;
    }
    yaml::Node root;
    try {
      root = yaml::parse(*text);
    } catch (const yaml::ParseError& error) {
      problems_.push_back(name + ": " + error.what());
      return;
    }
    const yaml::Node* areas = root.find("areas");
    if (!areas) {
      problems_.push_back(name + ": has no areas");
      return;
    }
    read_areas(*areas, scenario);
  }

  void read_groups(const yaml::Node& node, Scenario& scenario) {
    if (!node.is_list()) {
      problem(node, "groups must be a list");
      return;
    }
    for (const yaml::Node& item : node.list) {
      if (!item.is_map()) {
        problem(item, "a group must be a map");
        continue;
      }
      Group group;
      group.line = item.line;
      for (const auto& [key, value] : item.map) {
        if (key == "name") {
          group.name = scalar(value, key);
        } else if (key == "count") {
          const auto count = parse_number(scalar(value, key));
          if (!count || *count < 1 || *count > 5000 || *count != std::floor(*count)) {
            problem(value, "count must be a whole number of bots, 1..5000");
          } else {
            group.count = static_cast<unsigned>(*count);
          }
        } else if (key == "spawn") {
          read_spawn(value, group);
        } else if (key == "behavior") {
          group.behavior = scalar(value, key);
        } else if (key == "params") {
          if (!value.is_map() && value.kind != yaml::Node::Kind::Null) {
            problem(value, "params must be a map");
          }
          group.params = value;
        } else if (key == "network") {
          group.network = network(value);
        } else {
          problem(value, "unknown group setting " + key);
        }
      }
      scenario.groups.push_back(std::move(group));
    }
  }

  void read_spawn(const yaml::Node& node, Group& group) {
    if (!node.is_map()) {
      problem(node, "spawn must be a map ({ area: name, radius: r })");
      return;
    }
    for (const auto& [key, value] : node.map) {
      if (key == "area") {
        group.area = scalar(value, key);
      } else if (key == "radius") {
        const auto radius = parse_number(scalar(value, key));
        if (!radius || *radius < 0.0 || *radius > 2000.0) {
          problem(value, "spawn radius must be a number of metres, 0..2000");
        } else {
          group.spawn_radius = *radius;
        }
      } else {
        problem(value, "unknown spawn setting " + key);
      }
    }
  }

  void check_groups(const Scenario& scenario) {
    std::vector<std::string> names;
    for (const Group& group : scenario.groups) {
      yaml::Node at;
      at.line = group.line;
      if (group.name.empty()) {
        problem(at, "a group has no name");
      } else if (std::find(names.begin(), names.end(), group.name) != names.end()) {
        problem(at, "group name given twice: " + group.name);
      }
      names.push_back(group.name);
      if (group.count == 0) {
        problem(at, "group " + group.name + " has no count");
      }
      const auto& behaviors = known_behaviors();
      if (std::find(behaviors.begin(), behaviors.end(), group.behavior) == behaviors.end()) {
        std::string known;
        for (const std::string& name : behaviors) {
          known += (known.empty() ? "" : ", ") + name;
        }
        problem(at, "group " + group.name + " behavior must be one of " + known);
      }
      if (!group.area.empty() && !scenario.areas.count(group.area)) {
        problem(at, "group " + group.name + " goes to an unknown area " + group.area);
      }
    }
  }

  FileReader read_file_;
  std::vector<std::string> problems_;
};

// A scenario file from disk; areas_file is read next to it.
inline Scenario read_scenario_file(const std::string& path) {
  auto read_text = [](const std::string& file) -> std::optional<std::string> {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
      return std::nullopt;
    }
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
  };
  const auto text = read_text(path);
  if (!text) {
    throw ScenarioError({path + ": cannot be read"});
  }
  const std::size_t slash = path.find_last_of("/\\");
  const std::string folder = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
  ScenarioReader reader([&](const std::string& name) { return read_text(folder + name); });
  return reader.read(*text);
}

}  // namespace kcd2s::local::bots
