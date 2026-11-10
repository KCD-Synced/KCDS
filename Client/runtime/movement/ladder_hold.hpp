// Which clip a replica's ladder hold may set to the time its owner reports.
//
// The owner reports the time of the climb clip its game holds by hand (posture ladder_progress),
// and the clips its game starts arrive in their own messages. At the top of a ladder the game
// starts the climb-over clip, which it does not hold, and that start can arrive while the posture
// still reports the climb's time (for a few frames, still moving): the replica then held the new
// clip at the climb's time (40 % into the climb-over, live), and it went on from there, crouching
// over the edge while still low on the ladder (through the wall) and standing up a second early.
//
// On one ladder the game holds one clip by hand, the climb, both up and down. A clip whose
// reported time changes while it is held is that clip; from then on it is the only one held until
// no time is reported any more (off the ladder), and another clip plays on the game's own clock
// from its start. Before that (the grab, whose clip may still play when the first time comes),
// the held clip follows the owner's clips as before.
#pragma once

namespace kcds::runtime::movement {

class LadderHoldRule {
 public:
  // Whether `clip` may take the reported `time` now.
  bool may_hold(int clip, float time) {
    if (climb_ >= 0) {
      return clip == climb_;
    }
    if (clip != candidate_) {
      candidate_ = clip;
      first_time_ = time;
    } else if (time != first_time_) {
      climb_ = clip;
    }
    return true;
  }

  // No time is reported any more (off the ladder, or no clip held).
  void reset() {
    candidate_ = -1;
    climb_ = -1;
  }

 private:
  int candidate_ = -1;
  float first_time_ = 0.0f;
  int climb_ = -1;
};

}  // namespace kcds::runtime::movement
