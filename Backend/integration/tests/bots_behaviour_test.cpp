// kcds-bots behaviours (local/bots/behaviours): the same seed gives the same run, speeds stay
// within the server's movement check, and each behaviour does what its settings say.
#include "../local/bots/behaviours/catalog.hpp"

#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include "kcd2s/integration/movement_check.hpp"
#include "kcd2s/testkit.hpp"

using namespace kcd2s::local::bots;
using kcd2s::core::Vec3;

namespace {

// BotRandom(42, 0)'s first number, worked out once; every platform must give it.
constexpr std::uint64_t kReferenceFirst = 322574185352333729ull;

Group group_of(const std::string& behaviour, const std::string& params = "") {
  Scenario scenario = ScenarioReader().read(
      "name: t\ngroups:\n  - name: me\n    count: 1\n    behavior: " + behaviour +
      (params.empty() ? std::string() : "\n    params: " + params) +
      "\n  - name: them\n    count: 1\n    behavior: idle\n");
  return scenario.groups[0];
}

// A behaviour whose settings are right (a problem fails the test with its text).
std::unique_ptr<Behaviour> make(const std::string& behaviour, const std::string& params = "") {
  std::vector<std::string> problems;
  auto made = make_behaviour(group_of(behaviour, params), problems);
  if (!problems.empty()) {
    throw std::runtime_error(problems.front());
  }
  return made;
}

// Runs a behaviour for `seconds` at 30 ticks a second, moving the bot as the bot would (towards
// its intent at its speed), and calls `see` with every intent.
template <class See>
Vec3 run(Behaviour& behaviour, BotRandom& random, double seconds, Situation situation, See see) {
  const double tick = 1.0 / 30.0;
  for (double now = 0.0; now < seconds; now += tick) {
    situation.now = now;
    const Intent intent = behaviour.step(situation, random);
    see(situation, intent);
    const double gap = ground_distance(situation.position, intent.move_to);
    const double step = std::min(gap, intent.speed * tick);
    if (gap > 0.0 && step > 0.0) {
      situation.position.x += float((intent.move_to.x - situation.position.x) / gap * step);
      situation.position.y += float((intent.move_to.y - situation.position.y) / gap * step);
    }
  }
  return situation.position;
}

Situation at_home() {
  Situation situation;
  situation.home = {100.0f, 200.0f, 30.0f};
  situation.position = situation.home;
  situation.home_radius = 20.0;
  return situation;
}

}  // namespace

KCD2S_TEST(bots_behaviour, the_same_seed_runs_the_same_way) {
  for (const char* name : {"idle", "wander", "voice", "join_leave"}) {
    auto first = make(name);
    auto second = make(name);
    BotRandom one(42, 7);
    BotRandom two(42, 7);
    std::vector<double> a;
    std::vector<double> b;
    const Vec3 end_a = run(*first, one, 120.0, at_home(), [&](const Situation&, const Intent& i) {
      a.push_back(i.move_to.x + i.speed + (i.talking ? 1 : 0) + (i.chat ? 2 : 0));
    });
    const Vec3 end_b = run(*second, two, 120.0, at_home(), [&](const Situation&, const Intent& i) {
      b.push_back(i.move_to.x + i.speed + (i.talking ? 1 : 0) + (i.chat ? 2 : 0));
    });
    KCD2S_CHECK(a == b);
    KCD2S_CHECK(end_a.x == end_b.x && end_a.y == end_b.y);
  }
  // Another bot of the same seed runs differently.
  auto first = make("wander");
  auto other = make("wander");
  BotRandom one(42, 1);
  BotRandom two(42, 2);
  const Vec3 a = run(*first, one, 60.0, at_home(), [](const Situation&, const Intent&) {});
  const Vec3 b = run(*other, two, 60.0, at_home(), [](const Situation&, const Intent&) {});
  KCD2S_CHECK(a.x != b.x || a.y != b.y);
}

// The numbers are made by hand from mt19937_64, so a seed gives the same run on every standard
// library (the Mac's libc++ and Linux's libstdc++).
KCD2S_TEST(bots_behaviour, the_random_numbers_are_the_same_everywhere) {
  BotRandom random(42, 0);
  const double first = random.unit();
  const double second = random.unit();
  BotRandom again(42, 0);
  KCD2S_CHECK(again.unit() == first && again.unit() == second);
  KCD2S_CHECK(first >= 0.0 && first < 1.0);
  // The reference value of seed 42, bot 0 (the same on every platform).
  BotRandom reference(42, 0);
  std::uint64_t value = reference.next();
  KCD2S_CHECK_EQ(value, kReferenceFirst);
}

KCD2S_TEST(bots_behaviour, wanderers_stay_in_their_area_and_under_the_speed_limit) {
  auto wander = make("wander", "{ run_chance: 0.5, chat_every: [5s, 10s] }");
  BotRandom random(3, 3);
  double fastest = 0.0;
  double farthest = 0.0;
  int chats = 0;
  run(*wander, random, 300.0, at_home(), [&](const Situation& s, const Intent& i) {
    fastest = std::max(fastest, i.speed);
    farthest = std::max(farthest, ground_distance(s.position, s.home));
    chats += i.chat ? 1 : 0;
  });
  KCD2S_CHECK(fastest > 0.0 && fastest < double(kcd2s::integration::kMaxFootSpeed));
  KCD2S_CHECK(farthest <= 20.0 + 0.5);
  KCD2S_CHECK(chats >= 25 && chats <= 65);  // every 5..10 s for 300 s
}

KCD2S_TEST(bots_behaviour, a_bot_away_from_its_area_goes_there_first) {
  auto wander = make("wander");
  BotRandom random(1, 1);
  Situation situation = at_home();
  situation.position = {0.0f, 0.0f, 30.0f};  // 224 m away
  const Vec3 end = run(*wander, random, 120.0, situation, [](const Situation&, const Intent&) {});
  KCD2S_CHECK(ground_distance(end, situation.home) <= 20.5);
}

KCD2S_TEST(bots_behaviour, voice_talks_its_fraction_of_the_time) {
  auto voice = make("voice", "{ talk_fraction: 25%, burst: [1s, 3s] }");
  BotRandom random(9, 9);
  int talking = 0;
  int ticks = 0;
  run(*voice, random, 1200.0, at_home(), [&](const Situation&, const Intent& i) {
    talking += i.talking ? 1 : 0;
    ++ticks;
  });
  const double fraction = double(talking) / double(ticks);
  KCD2S_CHECK(fraction > 0.18 && fraction < 0.32);
  auto silent = make("voice", "{ talk_fraction: 0 }");
  int spoke = 0;
  run(*silent, random, 60.0, at_home(),
      [&](const Situation&, const Intent& i) { spoke += i.talking ? 1 : 0; });
  KCD2S_CHECK_EQ(spoke, 0);
}

KCD2S_TEST(bots_behaviour, join_leave_leaves_after_its_stay) {
  auto bot = make("join_leave", "{ stay: [10s, 20s], away: [3s, 4s] }");
  BotRandom random(5, 5);
  std::vector<double> leaves;
  run(*bot, random, 25.0, at_home(), [&](const Situation& s, const Intent& i) {
    if (i.leave) {
      leaves.push_back(s.now);
      KCD2S_CHECK(i.rejoin_at - s.now >= 3.0 && i.rejoin_at - s.now <= 4.0);
    }
  });
  KCD2S_CHECK(!leaves.empty());
  KCD2S_CHECK(leaves[0] >= 10.0 && leaves[0] <= 20.1);
}

KCD2S_TEST(bots_behaviour, a_melee_fighter_closes_in_and_swings_at_its_interval) {
  auto fighter =
      make("melee_fighter", "{ target_group: them, attack_interval: [1s, 1s], clinch_chance: 0 }");
  BotRandom random(2, 2);
  std::vector<Neighbour> others{{77, "them", {130.0f, 200.0f, 30.0f}}, {78, "me", {101, 200, 30}}};
  Situation situation = at_home();
  situation.others = &others;
  std::vector<double> swings;
  double closest = 1e9;
  run(*fighter, random, 30.0, situation, [&](const Situation& s, const Intent& i) {
    KCD2S_CHECK(i.lock == 77);  // not its own group
    closest = std::min(closest, ground_distance(s.position, others[0].position));
    if (i.attack) {
      swings.push_back(s.now);
      KCD2S_CHECK(ground_distance(s.position, others[0].position) <= kSwingDistance);
    }
  });
  KCD2S_CHECK(closest <= kSwingDistance && closest >= kEngageDistance - 0.1);
  // 30 m away: runs to 12 m, walks the rest (about 10 s), then a swing a second.
  KCD2S_CHECK(swings.size() >= 15 && swings.size() <= 21);
  for (std::size_t i = 1; i < swings.size(); ++i) {
    KCD2S_CHECK(std::fabs(swings[i] - swings[i - 1] - 1.0) < 0.05);
  }
  KCD2S_CHECK(std::fabs(fighter->block_chance() - 0.4) < 1e-9);
}

KCD2S_TEST(bots_behaviour, an_archer_keeps_its_distance_and_shoots) {
  auto archer = make("archer",
                     "{ target_group: them, keep_distance: 25, draw_time: 1s, "
                     "shot_interval: [2s, 2s] }");
  BotRandom random(4, 4);
  std::vector<Neighbour> others{{90, "them", {105.0f, 200.0f, 30.0f}}};
  Situation situation = at_home();
  situation.others = &others;
  int shots = 0;
  double last_distance = 0.0;
  run(*archer, random, 40.0, situation, [&](const Situation& s, const Intent& i) {
    last_distance = ground_distance(s.position, others[0].position);
    if (i.shoot) {
      ++shots;
      KCD2S_CHECK(i.aim);
      KCD2S_CHECK(last_distance >= 20.0 - 0.5 && last_distance <= 40.0 + 0.5);
    }
  });
  KCD2S_CHECK(last_distance >= 19.5);
  KCD2S_CHECK(shots >= 8);  // a shot every 3 s once in place
}

KCD2S_TEST(bots_behaviour, a_rider_asks_for_a_horse_mounts_and_gallops_under_the_limit) {
  auto rider = make("rider", "{ gait: gallop, route_radius: 50 }");
  BotRandom random(6, 6);
  Situation situation = at_home();
  Intent intent = rider->step(situation, random);
  KCD2S_CHECK(intent.want_horse);
  situation.has_horse = true;
  // A horse away from it: it goes there first, and mounts beside it.
  situation.horse = {situation.position.x + 20.0f, situation.position.y, situation.position.z};
  intent = rider->step(situation, random);
  KCD2S_CHECK(!intent.mount && intent.speed > 0.0);
  KCD2S_CHECK(ground_distance(intent.move_to, situation.horse) < kMountDistance);
  situation.horse = {situation.position.x + 1.5f, situation.position.y, situation.position.z};
  intent = rider->step(situation, random);
  KCD2S_CHECK(intent.mount);
  situation.mounted = true;
  double fastest = 0.0;
  run(*rider, random, 60.0, situation, [&](const Situation& s, const Intent& i) {
    fastest = std::max(fastest, i.speed);
    KCD2S_CHECK(ground_distance(s.position, s.home) <= 55.0);
  });
  KCD2S_CHECK(fastest > double(kcd2s::integration::kMaxFootSpeed));
  KCD2S_CHECK(fastest < double(kcd2s::integration::kMaxMountedSpeed));
}

KCD2S_TEST(bots_behaviour, settings_are_checked) {
  Scenario scenario = ScenarioReader().read(
      "name: t\ngroups:\n"
      "  - { name: a, count: 1, behavior: melee_fighter, params: { target_group: ghosts } }\n"
      "  - { name: b, count: 1, behavior: archer }\n"
      "  - { name: c, count: 1, behavior: wander, params: { run_chance: 3, colour: red } }\n"
      "  - { name: d, count: 1, behavior: rider, params: { gait: canter } }\n");
  const auto problems = check_behaviours(scenario);
  auto has = [&](const std::string& part) {
    for (const auto& problem : problems) {
      if (problem.find(part) != std::string::npos) {
        return true;
      }
    }
    return false;
  };
  KCD2S_CHECK(has("group a: target_group ghosts is not a group"));
  KCD2S_CHECK(has("group b: archer needs target_group"));
  KCD2S_CHECK(has("run_chance must be a chance"));
  KCD2S_CHECK(has("wander has no setting colour"));
  KCD2S_CHECK(has("gait must be one of gallop, trot"));
}

#ifdef KCDS_BOTS_SCENARIOS
// Every scenario that comes with kcds-bots reads, and its behaviours' settings are right.
KCD2S_TEST(bots_behaviour, the_shipped_scenarios_are_valid) {
  const std::string folder = KCDS_BOTS_SCENARIOS;
  for (const char* name : {"smoke", "town", "battle", "cavalry", "join_storm", "soak"}) {
    std::vector<std::string> problems;
    try {
      const Scenario scenario = read_scenario_file(folder + "/" + name + ".yaml");
      problems = check_behaviours(scenario);
      KCD2S_CHECK(scenario.bot_count() > 0);
      KCD2S_CHECK_EQ(scenario.name, std::string(name));
    } catch (const ScenarioError& error) {
      problems = error.problems();
    }
    for (const auto& problem : problems) {
      std::fprintf(stderr, "%s.yaml: %s\n", name, problem.c_str());
    }
    KCD2S_CHECK(problems.empty());
  }
  // The spec's sizes.
  KCD2S_CHECK_EQ(read_scenario_file(folder + "/town.yaml").bot_count(), 150u);
  KCD2S_CHECK_EQ(read_scenario_file(folder + "/battle.yaml").bot_count(), 120u);
  KCD2S_CHECK_EQ(read_scenario_file(folder + "/cavalry.yaml").bot_count(), 60u);
  KCD2S_CHECK_EQ(read_scenario_file(folder + "/join_storm.yaml").bot_count(), 300u);
  KCD2S_CHECK_EQ(read_scenario_file(folder + "/soak.yaml").bot_count(), 100u);
  KCD2S_CHECK_EQ(read_scenario_file(folder + "/smoke.yaml").bot_count(), 20u);
  KCD2S_CHECK_EQ(read_scenario_file(folder + "/soak.yaml").duration_s, 4.0 * 3600.0);
  KCD2S_CHECK_EQ(read_scenario_file(folder + "/join_storm.yaml").join_window_s, 30.0);
}
#endif
