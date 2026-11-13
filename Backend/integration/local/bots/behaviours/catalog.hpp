#pragma once
// The behaviours by the name a scenario gives them (scenario.hpp known_behaviors).
#include <memory>
#include <string>
#include <vector>

#include "combat.hpp"
#include "riding.hpp"
#include "social.hpp"

namespace kcd2s::local::bots {

// A new behaviour for one bot of `group`; its settings' problems go to `problems` (a scenario is
// checked once by making each group's behaviour before the run).
inline std::unique_ptr<Behaviour> make_behaviour(const Group& group,
                                                 std::vector<std::string>& problems) {
  Params params(group.params, group.behavior, problems);
  std::unique_ptr<Behaviour> behaviour;
  if (group.behavior == "idle") {
    behaviour = std::make_unique<IdleBehaviour>(params);
  } else if (group.behavior == "wander") {
    behaviour = std::make_unique<WanderBehaviour>(params);
  } else if (group.behavior == "melee_fighter") {
    behaviour = std::make_unique<MeleeFighterBehaviour>(params, problems);
  } else if (group.behavior == "archer") {
    behaviour = std::make_unique<ArcherBehaviour>(params, problems);
  } else if (group.behavior == "rider") {
    behaviour = std::make_unique<RiderBehaviour>(params);
  } else if (group.behavior == "voice") {
    behaviour = std::make_unique<VoiceBehaviour>(params);
  } else if (group.behavior == "join_leave") {
    behaviour = std::make_unique<JoinLeaveBehaviour>(params);
  } else {
    problems.push_back("unknown behavior " + group.behavior);
    return nullptr;
  }
  params.finish();
  return behaviour;
}

// Every group's behaviour settings, checked once; empty when all are right.
inline std::vector<std::string> check_behaviours(const Scenario& scenario) {
  std::vector<std::string> problems;
  for (const Group& group : scenario.groups) {
    std::vector<std::string> own;
    make_behaviour(group, own);
    for (const std::string& problem : own) {
      problems.push_back("group " + group.name + ": " + problem);
    }
    // Fighters' targets are groups of this scenario.
    if (const yaml::Node* targets = group.params.find("target_group")) {
      std::vector<std::string> names;
      if (targets->is_scalar()) {
        names.push_back(targets->scalar);
      }
      for (const yaml::Node& item : targets->list) {
        names.push_back(item.scalar);
      }
      for (const std::string& name : names) {
        if (!scenario.group(name)) {
          problems.push_back("group " + group.name + ": target_group " + name +
                             " is not a group of this scenario");
        }
      }
    }
  }
  return problems;
}

}  // namespace kcd2s::local::bots
