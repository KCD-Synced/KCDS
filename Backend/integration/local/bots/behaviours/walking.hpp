#pragma once
// Walking about an area: to a random point of it, a pause there, then the next point. The idle,
// wander, voice and join_leave behaviours share it.
#include "behaviour.hpp"

namespace kcd2s::local::bots {

class AreaWalk {
 public:
  struct Settings {
    Range pause{2.0, 8.0};    // standing at each point
    double run_chance = 0.2;  // the next leg is run instead of walked
    bool moves = true;        // false: stands where it is (idle)
  };

  explicit AreaWalk(Settings settings) : settings_(settings) {
  }

  // Fills the movement of `intent`.
  void step(const Situation& situation, BotRandom& random, Intent& intent) {
    if (!started_) {
      started_ = true;
      target_ = situation.position;
      pause_until_ = situation.now + random.between(settings_.pause);
      // A bot outside its area first goes there (it joined where the server spawned it).
      if (ground_distance(situation.position, situation.home) > situation.home_radius) {
        target_ = random.point_near(situation.home, situation.home_radius);
        speed_ = kRunSpeed;
        walking_ = true;
      }
    }
    if (walking_) {
      if (ground_distance(situation.position, target_) < 0.5) {
        walking_ = false;
        pause_until_ = situation.now + random.between(settings_.pause);
      }
    } else if (settings_.moves && situation.now >= pause_until_) {
      target_ = random.point_near(situation.home, situation.home_radius);
      speed_ = random.chance(settings_.run_chance) ? kRunSpeed : kWalkSpeed;
      walking_ = true;
    }
    intent.move_to = walking_ ? target_ : situation.position;
    intent.speed = walking_ ? speed_ : 0.0;
  }

  bool walking() const {
    return walking_;
  }

 private:
  Settings settings_;
  bool started_ = false;
  bool walking_ = false;
  core::Vec3 target_;
  double speed_ = kWalkSpeed;
  double pause_until_ = 0.0;
};

}  // namespace kcd2s::local::bots
