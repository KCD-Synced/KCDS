// A whip's lash as a whip moves (Asset/Animation Studio): our own model of a
// tapered lash, because the game's rope (CRopeEntity) has one mass and one thickness along its
// length. Such a rope only hangs and swings from the hand (measured over MCP with recorded keys:
// its tip never outran the hand, 0.6 times its speed); a whip cracks because it gets lighter
// toward its tip, so the wave the hand starts runs faster and faster down it (the same hand path
// in this model: 2.7 times the hand's speed, the lash out straight behind on the back cast and
// in front on the strike).
//
// Position-based dynamics: the lash's points (start first) move with their speed (each pulled
// toward its neighbours', so the lash stays a smooth curve), gravity and air drag; each segment
// keeps its length, the lighter point giving more; each point keeps two segments from the one
// two before it (bending: a straight lash), strongly at the handle and not at all at the tip;
// the first segment leaves the handle along its long side. The start is put at the handle's
// tip, moving evenly from where it was to where it is over the frame, in steps of 1/300 s.
// Points stay above the ground height given. Tuned offline on a recorded strike (stroboscope
// of the points): the lash trails behind on the back cast, loops over the hand, rolls out
// straight in front and then falls.
//
// Engine thread.
#pragma once

#include <array>
#include <vector>

namespace kcds::runtime::physical_props {

// A body part the lash collides with (Ignore characters off): a capsule from a to b (world
// metres) `radius` round.
struct LashBody {
  float a[3] = {0, 0, 0};
  float b[3] = {0, 0, 0};
  float radius = 0.1f;
  // Whether it is the holder's own body (the others: players and NPCs near).
  bool own = false;
};

struct WhipLashSettings {
  // m/s², downwards.
  float gravity = 9.8f;
  // How much speed the air takes each second (the lash's own drag plus the rope's damping).
  float drag = 4.0f;
  // The tip's weight as a share of the weight at the handle.
  float taper = 0.08f;
  // How straight the lash keeps at the handle (0 limp, 1 stiff); it weakens to none at the tip.
  float stiffness = 0.4f;
  // Whether it stays above the ground height.
  bool ground = true;
  // The lash posed instead of moved by physics, to animate a swing exactly (prop keys blend
  // these along an animation): shape 0 is physics alone, 1 the posed shape alone (no gravity);
  // between, each step pulls the lash toward the shape (shape³ of the way at 300 steps a second:
  // 0.5 follows it closely, 0.2 within half a second, 0.1 barely), so going from 1 to 0 over keys
  // lets it go with the speed it had. The shape is a smooth bend from the handle to the tip:
  // curl degrees in all toward the handle's up side (negative: down), sway degrees sideways (to
  // its right side, negative: left); 0 and 0 is straight out of the handle.
  float shape = 0.0f;
  float curl = 0.0f;
  float sway = 0.0f;
  // The bodies it collides with (null or none: it passes through them); its points stay outside
  // each capsule, the two at the handle left out.
  const std::vector<LashBody>* bodies = nullptr;
};

class WhipLash {
 public:
  // A lash of `segments` segments, `length` metres, hanging from `start`.
  void reset(int segments, float length, const float start[3]);

  // One frame of `seconds`: the start goes to `start` (the handle's tip) with the handle along
  // `direction` (a unit vector from the hand to the tip) and its up side `up` (a unit vector
  // across it: the shape's curl bends toward it); nothing below `ground` (metres up).
  void step(const float start[3], const float direction[3], const float up[3], float seconds,
            float ground, const WhipLashSettings& settings);

  // The posed shape's points (WhipLashSettings shape) from `start` along `direction` with `up`.
  void posed(const float start[3], const float direction[3], const float up[3],
             const WhipLashSettings& settings, std::vector<float>& points) const;

  // One frame in a keyed shape instead of physics (an animation's lash keys): the
  // shape's points from the handle's tip in its frame (along `direction`, to its right, toward
  // `up`), the lash's own points laid along them a segment apart (past the shape's end straight
  // on). The points before are kept as the last frame's, so physics goes on with their speed
  // when the keys stop.
  void shaped(const float start[3], const float direction[3], const float up[3],
              const std::vector<std::array<float, 3>>& shape);

  // The points now, start first, x y z each (segments + 1 of them).
  const std::vector<float>& points() const {
    return points_;
  }

  bool ready() const {
    return !points_.empty();
  }

 private:
  std::vector<float> points_;
  std::vector<float> before_;
  std::vector<float> weights_;
  // A step's speeds and the same pulled toward the neighbours' (kept to save allocations).
  std::vector<float> speeds_;
  std::vector<float> smoothed_;
  std::vector<float> shaped_;
  float segment_ = 0.0f;
  float start_[3] = {0, 0, 0};
  float direction_[3] = {0, 0, -1};
  float up_[3] = {1, 0, 0};
};

}  // namespace kcds::runtime::physical_props
