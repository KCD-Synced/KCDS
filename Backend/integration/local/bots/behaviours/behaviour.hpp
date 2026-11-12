#pragma once
// What a bot does, as a small state machine: every tick it gets what the bot sees (Situation) and
// says what it wants (Intent): where to walk and how fast, whom to fight, when to swing, aim,
// shoot, talk, ride or leave. The bot (bot/) turns an intent into the messages a real game client
// sends; a behaviour itself sends nothing, so it is deterministic for one seed and tested alone.
//
// Speeds stay within what the server's movement check allows (integration/movement_check.hpp):
// on foot at most a sprint, mounted at most a gallop, both well under the limits.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "../scenario/scenario.hpp"
#include "kcd2s/core/ids.hpp"
#include "kcd2s/core/math.hpp"

namespace kcd2s::local::bots {

// Speeds in metres a second, below the server's limits (10 on foot, 22 mounted).
inline constexpr double kWalkSpeed = 1.6;
inline constexpr double kRunSpeed = 4.5;
inline constexpr double kSprintSpeed = 6.5;
inline constexpr double kTrotSpeed = 6.0;
inline constexpr double kGallopSpeed = 14.0;

// Another player the bot sees: a bot's group comes from its nickname (bot-<group>-<n>), a real
// player has none.
struct Neighbour {
  core::EntityId id = 0;
  std::string group;
  core::Vec3 position;
};

struct Situation {
  double now = 0.0;  // seconds since the run started
  core::Vec3 position;
  core::Vec3 home;  // the centre of the bot's area, in the world
  double home_radius = 10.0;
  const std::vector<Neighbour>* others = nullptr;
  bool mounted = false;
  bool has_horse = false;  // the server gave it a horse (rider)
  core::Vec3 horse;        // where that horse stands
};

struct Intent {
  // Movement: towards `move_to` at `speed` (0 stands still).
  core::Vec3 move_to;
  double speed = 0.0;
  bool face_point = false;  // face `face` instead of where it walks
  core::Vec3 face;
  // Melee: the locked opponent (0: none), a swing that starts now, a held block.
  core::EntityId lock = 0;
  bool attack = false;
  bool block = false;
  bool clinch = false;
  // Ranged: drawing and aiming at `face`, and an arrow loosed now.
  bool aim = false;
  bool shoot = false;
  // Riding: ask for a horse, mount it, and ride.
  bool want_horse = false;
  bool mount = false;
  // Talking in voice chat this tick.
  bool talking = false;
  // A chat line or an emote that starts now.
  std::optional<std::string> chat;
  bool emote = false;
  // join_leave: leave the server now (the bot joins again when `rejoin_at` comes).
  bool leave = false;
  double rejoin_at = 0.0;
};

// The seeded random numbers of one bot: the same scenario seed and bot index give the same run, on
// every platform (std::mt19937_64 is the same everywhere; the standard distributions are not, so
// the numbers are made from its output here).
class BotRandom {
 public:
  BotRandom(std::uint64_t seed, std::uint64_t bot) : engine_(mix(seed ^ mix(bot + 1))) {
  }
  // A number in [0, 1): the top 53 bits of the next output.
  double unit() {
    return double(engine_() >> 11) * (1.0 / 9007199254740992.0);
  }
  double uniform(double low, double high) {
    if (!(high > low)) {
      return low;
    }
    return low + (high - low) * unit();
  }
  double between(const Range& range) {
    return uniform(range.min, range.max);
  }
  bool chance(double probability) {
    return unit() < probability;
  }
  std::uint64_t next() {
    return engine_();
  }
  // A point within `radius` of `centre` on the ground plane, spread evenly over the disc.
  core::Vec3 point_near(const core::Vec3& centre, double radius) {
    const double angle = uniform(0.0, 2.0 * 3.14159265358979323846);
    const double distance = radius * std::sqrt(uniform(0.0, 1.0));
    return {float(centre.x + distance * std::cos(angle)),
            float(centre.y + distance * std::sin(angle)), centre.z};
  }

 private:
  static std::uint64_t mix(std::uint64_t value) {
    // splitmix64: neighbouring seeds give unrelated streams.
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
  }
  std::mt19937_64 engine_;
};

inline double ground_distance(const core::Vec3& a, const core::Vec3& b) {
  const double dx = double(a.x) - double(b.x);
  const double dy = double(a.y) - double(b.y);
  return std::sqrt(dx * dx + dy * dy);
}

// A behaviour's settings (a group's params), read with defaults; every unknown or invalid one is a
// problem the scenario's author sees.
class Params {
 public:
  Params(const yaml::Node& node, std::string behaviour, std::vector<std::string>& problems)
      : node_(node), behaviour_(std::move(behaviour)), problems_(problems) {
  }

  double number(const std::string& key, double fallback, double low, double high) {
    used_.push_back(key);
    const yaml::Node* value = node_.find(key);
    if (!value) {
      return fallback;
    }
    const auto parsed = value->is_scalar() ? parse_number(value->scalar) : std::nullopt;
    if (!parsed || *parsed < low || *parsed > high) {
      problem(*value, key + " must be a number from " + text(low) + " to " + text(high));
      return fallback;
    }
    return *parsed;
  }
  double chance(const std::string& key, double fallback) {
    used_.push_back(key);
    const yaml::Node* value = node_.find(key);
    if (!value) {
      return fallback;
    }
    const auto parsed = value->is_scalar() ? parse_fraction(value->scalar) : std::nullopt;
    if (!parsed) {
      problem(*value, key + " must be a chance from 0 to 1 (or a percentage)");
      return fallback;
    }
    return *parsed;
  }
  double seconds(const std::string& key, double fallback) {
    used_.push_back(key);
    const yaml::Node* value = node_.find(key);
    if (!value) {
      return fallback;
    }
    const auto parsed = value->is_scalar() ? parse_seconds(value->scalar) : std::nullopt;
    if (!parsed) {
      problem(*value, key + " must be a time such as 1.5s");
      return fallback;
    }
    return *parsed;
  }
  // A time range: [min, max], or one time for both.
  Range interval(const std::string& key, Range fallback) {
    used_.push_back(key);
    const yaml::Node* value = node_.find(key);
    if (!value) {
      return fallback;
    }
    if (value->is_scalar()) {
      const auto one = parse_seconds(value->scalar);
      if (one) {
        return {*one, *one};
      }
    } else if (value->is_list() && value->list.size() == 2 && value->list[0].is_scalar() &&
               value->list[1].is_scalar()) {
      const auto low = parse_seconds(value->list[0].scalar);
      const auto high = parse_seconds(value->list[1].scalar);
      if (low && high && *low <= *high) {
        return {*low, *high};
      }
    }
    problem(*value, key + " must be [min, max] times such as [0.8s, 2s]");
    return fallback;
  }
  // One name or a list of names.
  std::vector<std::string> names(const std::string& key) {
    used_.push_back(key);
    const yaml::Node* value = node_.find(key);
    if (!value) {
      return {};
    }
    if (value->is_scalar()) {
      return {value->scalar};
    }
    std::vector<std::string> out;
    if (value->is_list()) {
      for (const yaml::Node& item : value->list) {
        if (!item.is_scalar()) {
          problem(item, key + " must be names");
          return {};
        }
        out.push_back(item.scalar);
      }
      return out;
    }
    problem(*value, key + " must be a name or a list of names");
    return {};
  }
  std::string word(const std::string& key, const std::string& fallback,
                   const std::vector<std::string>& allowed) {
    used_.push_back(key);
    const yaml::Node* value = node_.find(key);
    if (!value) {
      return fallback;
    }
    if (value->is_scalar() && (allowed.empty() || std::find(allowed.begin(), allowed.end(),
                                                            value->scalar) != allowed.end())) {
      return value->scalar;
    }
    std::string list;
    for (const std::string& one : allowed) {
      list += (list.empty() ? "" : ", ") + one;
    }
    problem(*value, key + (allowed.empty() ? " must be a word" : " must be one of " + list));
    return fallback;
  }
  // Call after reading every setting: what the behaviour does not know is a problem.
  void finish() {
    for (const auto& [key, value] : node_.map) {
      if (std::find(used_.begin(), used_.end(), key) == used_.end()) {
        problem(value, behaviour_ + " has no setting " + key);
      }
    }
  }

 private:
  void problem(const yaml::Node& node, const std::string& what) {
    problems_.push_back("line " + std::to_string(node.line) + ": " + behaviour_ + " " + what);
  }
  static std::string text(double value) {
    std::string out = std::to_string(value);
    while (out.find('.') != std::string::npos && (out.back() == '0' || out.back() == '.')) {
      const bool dot = out.back() == '.';
      out.pop_back();
      if (dot) {
        break;
      }
    }
    return out;
  }

  const yaml::Node& node_;
  std::string behaviour_;
  std::vector<std::string>& problems_;
  std::vector<std::string> used_;
};

class Behaviour {
 public:
  virtual ~Behaviour() = default;
  // The intent for this tick.
  virtual Intent step(const Situation& situation, BotRandom& random) = 0;
  // Melee: the chance this bot blocks a blow that reaches it (its game's guard).
  virtual double block_chance() const {
    return 0.0;
  }
};

}  // namespace kcd2s::local::bots
