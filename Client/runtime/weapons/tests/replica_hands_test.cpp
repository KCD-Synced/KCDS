#include "../replica_hands.hpp"

#include <cstdio>

namespace {

using kcds::runtime::weapons::HandsState;
using kcds::runtime::weapons::HandsStep;
using kcds::runtime::weapons::next_hands_step;

int failures = 0;

void expect(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAILED: %s\n", what);
    ++failures;
  }
}

HandsState drawn_before() {
  HandsState state;
  state.shown = true;
  state.same_field = true;
  state.same_sets = true;
  return state;
}

}  // namespace

int main() {
  // A torch in the field while the replica carries: nothing is drawn until the carry ends.
  HandsState carrying = drawn_before();
  carrying.carrying = true;
  carrying.same_field = false;
  expect(next_hands_step(carrying) == HandsStep::wait, "a new field waits while carrying");
  carrying.same_field = true;
  expect(next_hands_step(carrying) == HandsStep::wait, "the drawn field waits while carrying");
  carrying.let_go = true;
  expect(next_hands_step(carrying) == HandsStep::wait, "a let-go waits while carrying");

  // The carry is over: the field that came during it is drawn.
  HandsState after = drawn_before();
  after.same_field = false;
  expect(next_hands_step(after) == HandsStep::draw, "a field that came during the carry draws");

  // Nothing changed: the hands keep what they hold.
  expect(next_hands_step(drawn_before()) == HandsStep::keep, "an unchanged field keeps");

  // The hands let go, the sets changed, or nothing was drawn yet: drawn again.
  HandsState let_go = drawn_before();
  let_go.let_go = true;
  expect(next_hands_step(let_go) == HandsStep::draw, "a let-go draws again");
  HandsState sets = drawn_before();
  sets.same_sets = false;
  expect(next_hands_step(sets) == HandsStep::draw, "changed sets draw again");
  expect(next_hands_step(HandsState{}) == HandsStep::draw, "the first field draws");

  if (failures == 0) {
    std::printf("replica_hands: all passed\n");
  }
  return failures == 0 ? 0 : 1;
}
