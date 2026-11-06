#pragma once
// When another player's body here has drifted from their replicated position long enough to be
// put there with the game's script SetWorldPos reason. Each frame the replica is asked to stand at
// its owner's position with a plain SetWorldTM, which the animated character often keeps to itself
// (it moves on its own movement request and collides here): indoors a replica stuck at a doorway
// or at the foot of the stairs while its owner walked up them, up to the 5 m teleport distance
// away, a floor apart. A replica more than kDriftMeters away for kDriftSeconds,
// or more than kDriftHeightMeters above or below, is put there at once.
#include <cmath>

namespace kcds::runtime::replicas {

class ReplicaDrift {
 public:
  static constexpr float kDriftMeters = 1.0f;
  static constexpr double kDriftSeconds = 0.5;
  static constexpr float kDriftHeightMeters = 1.5f;

  // `dx`, `dy`, `dz`: the replicated position less the body's (metres); `now`: seconds. True when
  // the body should be put at the replicated position this frame.
  bool should_place(float dx, float dy, float dz, double now) {
    if (std::fabs(dz) > kDriftHeightMeters) {
      since_ = -1.0;
      return true;
    }
    if (dx * dx + dy * dy + dz * dz <= kDriftMeters * kDriftMeters) {
      since_ = -1.0;
      return false;
    }
    if (since_ < 0.0) {
      since_ = now;
      return false;
    }
    if (now - since_ >= kDriftSeconds) {
      since_ = -1.0;
      return true;
    }
    return false;
  }

 private:
  // When the body first stood further than kDriftMeters away, -1 while it is near.
  double since_ = -1.0;
};

}  // namespace kcds::runtime::replicas
