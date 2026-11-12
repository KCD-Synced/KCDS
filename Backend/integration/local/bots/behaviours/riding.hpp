#pragma once
// rider: asks the server for a horse (the bots' support resource gives it one), mounts it and
// rides a round about its area at a gallop or a trot: a polygon of `corners` corners,
// `route_radius` metres from the area's centre. A rider whose horse never comes walks its area.
#include "combat.hpp"

namespace kcd2s::local::bots {

// How near its horse a rider stands to mount.
inline constexpr double kMountDistance = 2.5;

class RiderBehaviour : public Behaviour {
 public:
  explicit RiderBehaviour(Params& params)
      : gait_(params.word("gait", "gallop", {"gallop", "trot"})),
        route_radius_(params.number("route_radius", 120.0, 20.0, 1000.0)),
        corners_(int(params.number("corners", 8.0, 3.0, 64.0))),
        walk_({{2.0, 6.0}, 0.2, true}) {
  }
  Intent step(const Situation& situation, BotRandom& random) override {
    Intent intent;
    if (!situation.has_horse) {
      intent.want_horse = true;
      walk_.step(situation, random, intent);
      return intent;
    }
    if (!situation.mounted) {
      // To the horse first; a rider mounts beside it.
      if (ground_distance(situation.position, situation.horse) > kMountDistance) {
        intent.move_to = short_of(situation.position, situation.horse, kMountDistance - 0.5);
        intent.speed = kRunSpeed;
        return intent;
      }
      intent.mount = true;
      intent.move_to = situation.position;
      return intent;
    }
    if (corner_ < 0) {
      // The round starts at a random corner, in a random direction.
      corner_ = int(random.next() % unsigned(corners_));
      direction_ = random.chance(0.5) ? 1 : -1;
    }
    const core::Vec3 target = corner_point(situation.home, corner_);
    if (ground_distance(situation.position, target) < 3.0) {
      corner_ = (corner_ + direction_ + corners_) % corners_;
    }
    intent.move_to = corner_point(situation.home, corner_);
    intent.speed = gait_ == "gallop" ? kGallopSpeed : kTrotSpeed;
    return intent;
  }

 private:
  core::Vec3 corner_point(const core::Vec3& home, int corner) const {
    const double angle = 2.0 * 3.14159265358979323846 * double(corner) / double(corners_);
    return {float(home.x + route_radius_ * std::cos(angle)),
            float(home.y + route_radius_ * std::sin(angle)), home.z};
  }

  std::string gait_;
  double route_radius_;
  int corners_;
  AreaWalk walk_;
  int corner_ = -1;
  int direction_ = 1;
};

}  // namespace kcd2s::local::bots
