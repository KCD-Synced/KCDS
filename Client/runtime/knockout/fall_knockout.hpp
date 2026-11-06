// A fall that takes the last health knocks the player out (from 30 m a player stood with 1
// health and walked on). The game's fall damage is cut off at the immortality's minimum and no
// knock-out comes of it, as one does of a blow, so the landing is told by its numbers: health came
// down to that minimum just after the body was in the air. Health at that minimum for any other
// reason (a resource's setHealth(1)) is left alone.
#pragma once

#include <cstdint>

namespace kcds::runtime::knockout {

class FallKnockout {
 public:
  // rpg_param ImmortalHealthMin, and how far health may have regenerated since.
  static constexpr float kLastHealth = 1.05f;
  // How long after the air a landing's damage counts.
  static constexpr std::uint64_t kLandingMs = 1500;

  // Each frame. True once for a landing that took the last health.
  bool update(std::uint64_t now_ms, bool in_air, float health) {
    if (in_air) {
      air_at_ms_ = now_ms;
      was_in_air_ = true;
    }
    const bool landed = was_in_air_ && now_ms - air_at_ms_ <= kLandingMs;
    const bool lethal = landed && health <= kLastHealth && health_before_ > kLastHealth;
    health_before_ = health;
    return lethal;
  }

 private:
  std::uint64_t air_at_ms_ = 0;
  bool was_in_air_ = false;
  float health_before_ = 0.0f;
};

}  // namespace kcds::runtime::knockout
