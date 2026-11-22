// What a replica does with its hands each frame, against what its owner's "hands" field names.
//
// While the replica carries an item in its arms (carry/carry_items.hpp), its hands are the carry's:
// the game refuses a draw for its own player then ("busy"), so the replica draws nothing either.
// An owner's torch came back in its field while its replica still took the carried item up (the
// owner's carry field stays a moment after the carry ends), the replica drew the torch in the
// middle of the game's carry action, and both watching games failed a frame later in the game's
// AI grid. What the field names is drawn once the carry is over.
#pragma once

namespace kcds::runtime::weapons {

enum class HandsStep {
  wait,  // the hands are the carry's: nothing is drawn or put away
  keep,  // the hands already hold what the field names
  draw,  // the hands are made to hold what the field names
};

struct HandsState {
  bool carrying = false;    // the replica carries an item in its arms
  bool shown = false;       // the field was drawn before
  bool same_field = false;  // the field is the one drawn last
  bool same_sets = false;   // the weapon sets are the ones drawn last
  bool let_go = false;      // the hands let go of what was drawn
};

inline HandsStep next_hands_step(const HandsState& state) {
  if (state.carrying) {
    return HandsStep::wait;
  }
  if (state.shown && state.same_field && state.same_sets && !state.let_go) {
    return HandsStep::keep;
  }
  return HandsStep::draw;
}

}  // namespace kcds::runtime::weapons
