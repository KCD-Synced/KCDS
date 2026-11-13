#pragma once
// The fighting bots: melee_fighter closes in on the nearest bot of its target groups, locks on
// and swings at random intervals (now and then a clinch instead), and blocks part of the blows
// that reach it; archer keeps its distance from its targets, draws, aims and looses. Both walk
// about their area while no target is in sight.
#include <string>
#include <vector>

#include "walking.hpp"

namespace kcd2s::local::bots {

// How far a bot sees a target, and the reach it swings within (the server's kMeleeReach is 5 m;
// a bot closes to well inside it).
inline constexpr double kSightDistance = 80.0;
inline constexpr double kEngageDistance = 1.8;
inline constexpr double kSwingDistance = 2.6;

// The nearest neighbour whose group is one of `groups`, within `sight`; nullptr for none.
inline const Neighbour* nearest_of(const Situation& situation,
                                   const std::vector<std::string>& groups, double sight) {
  const Neighbour* nearest = nullptr;
  double best = sight;
  if (!situation.others) {
    return nullptr;
  }
  for (const Neighbour& other : *situation.others) {
    if (std::find(groups.begin(), groups.end(), other.group) == groups.end()) {
      continue;
    }
    const double distance = ground_distance(situation.position, other.position);
    if (distance <= best) {
      best = distance;
      nearest = &other;
    }
  }
  return nearest;
}

// The point `distance` short of `to` on the way from `from`.
inline core::Vec3 short_of(const core::Vec3& from, const core::Vec3& to, double distance) {
  const double gap = ground_distance(from, to);
  if (gap <= distance || gap <= 0.0) {
    return from;
  }
  const double keep = (gap - distance) / gap;
  return {float(from.x + (to.x - from.x) * keep), float(from.y + (to.y - from.y) * keep), to.z};
}

class MeleeFighterBehaviour : public Behaviour {
 public:
  MeleeFighterBehaviour(Params& params, std::vector<std::string>& problems)
      : targets_(params.names("target_group")),
        attack_interval_(params.interval("attack_interval", {0.8, 2.0})),
        block_chance_(params.chance("block_chance", 0.4)),
        clinch_chance_(params.chance("clinch_chance", 0.05)),
        walk_({{2.0, 6.0}, 0.3, true}) {
    if (targets_.empty()) {
      problems.push_back("melee_fighter needs target_group (a group name or a list of them)");
    }
  }
  Intent step(const Situation& situation, BotRandom& random) override {
    Intent intent;
    const Neighbour* target = nearest_of(situation, targets_, kSightDistance);
    if (!target) {
      walk_.step(situation, random, intent);
      return intent;
    }
    const double distance = ground_distance(situation.position, target->position);
    intent.lock = target->id;
    intent.face_point = true;
    intent.face = target->position;
    if (distance > kSwingDistance) {
      intent.move_to = short_of(situation.position, target->position, kEngageDistance);
      intent.speed = distance > 12.0 ? kRunSpeed : kWalkSpeed;
      return intent;
    }
    intent.move_to = situation.position;
    if (next_attack_ < 0.0) {
      next_attack_ = situation.now + random.between(attack_interval_);
    }
    if (situation.now >= next_attack_) {
      if (random.chance(clinch_chance_)) {
        intent.clinch = true;
      } else {
        intent.attack = true;
      }
      next_attack_ = situation.now + random.between(attack_interval_);
    }
    return intent;
  }
  double block_chance() const override {
    return block_chance_;
  }

 private:
  std::vector<std::string> targets_;
  Range attack_interval_;
  double block_chance_;
  double clinch_chance_;
  AreaWalk walk_;
  double next_attack_ = -1.0;
};

class ArcherBehaviour : public Behaviour {
 public:
  ArcherBehaviour(Params& params, std::vector<std::string>& problems)
      : targets_(params.names("target_group")),
        keep_distance_(params.number("keep_distance", 25.0, 5.0, 70.0)),
        draw_time_(params.seconds("draw_time", 1.2)),
        shot_interval_(params.interval("shot_interval", {3.0, 6.0})),
        walk_({{2.0, 6.0}, 0.2, true}) {
    if (targets_.empty()) {
      problems.push_back("archer needs target_group (a group name or a list of them)");
    }
  }
  Intent step(const Situation& situation, BotRandom& random) override {
    Intent intent;
    const Neighbour* target = nearest_of(situation, targets_, kSightDistance);
    if (!target) {
      drawing_since_ = -1.0;
      walk_.step(situation, random, intent);
      return intent;
    }
    intent.face_point = true;
    intent.face = target->position;
    const double distance = ground_distance(situation.position, target->position);
    if (distance < keep_distance_ - 5.0) {
      // Too close: step back away from it (any way when it stands right on it, as bots that
      // joined at one spawn point do).
      const double away = keep_distance_ - distance;
      double dx = situation.position.x - target->position.x;
      double dy = situation.position.y - target->position.y;
      double length = std::sqrt(dx * dx + dy * dy);
      if (length < 0.5) {
        const double angle = random.uniform(0.0, 2.0 * 3.14159265358979323846);
        dx = std::cos(angle);
        dy = std::sin(angle);
        length = 1.0;
      }
      intent.move_to = {float(situation.position.x + dx / length * away),
                        float(situation.position.y + dy / length * away), situation.position.z};
      intent.speed = kRunSpeed;
      drawing_since_ = -1.0;
      return intent;
    }
    if (distance > keep_distance_ + 15.0) {
      intent.move_to = short_of(situation.position, target->position, keep_distance_);
      intent.speed = kRunSpeed;
      drawing_since_ = -1.0;
      return intent;
    }
    intent.move_to = situation.position;
    if (situation.now < next_draw_) {
      return intent;
    }
    if (drawing_since_ < 0.0) {
      drawing_since_ = situation.now;
    }
    intent.aim = true;
    if (situation.now - drawing_since_ >= draw_time_) {
      intent.shoot = true;
      drawing_since_ = -1.0;
      next_draw_ = situation.now + random.between(shot_interval_);
    }
    return intent;
  }

 private:
  std::vector<std::string> targets_;
  double keep_distance_;
  double draw_time_;
  Range shot_interval_;
  AreaWalk walk_;
  double drawing_since_ = -1.0;
  double next_draw_ = 0.0;
};

}  // namespace kcd2s::local::bots
