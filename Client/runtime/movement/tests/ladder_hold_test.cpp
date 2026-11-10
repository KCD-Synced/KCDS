#include "../ladder_hold.hpp"

#include "kcd2s/testkit.hpp"

using kcds::runtime::movement::LadderHoldRule;

KCD2S_TEST(ladder_hold, the_grab_clip_gives_way_to_the_climb) {
  LadderHoldRule rule;
  // The first time comes while the grab's clip still plays, the climb's clip a frame later.
  KCD2S_CHECK(rule.may_hold(10638, 0.0f));
  KCD2S_CHECK(rule.may_hold(10635, 0.0f));
  KCD2S_CHECK(rule.may_hold(10635, 0.1f));
  KCD2S_CHECK(rule.may_hold(10635, 0.4f));
}

KCD2S_TEST(ladder_hold, the_climb_over_plays_on_its_own_clock) {
  LadderHoldRule rule;
  KCD2S_CHECK(rule.may_hold(10635, 0.0f));
  KCD2S_CHECK(rule.may_hold(10635, 0.2f));
  // The climb-over starts while the climb's time is still reported, and still moving.
  KCD2S_CHECK(!rule.may_hold(10640, 0.4f));
  KCD2S_CHECK(!rule.may_hold(10640, 0.49f));
  // Climbing down again on the same ladder holds the climb.
  KCD2S_CHECK(rule.may_hold(10635, 0.3f));
}

KCD2S_TEST(ladder_hold, the_next_ladder_starts_afresh) {
  LadderHoldRule rule;
  KCD2S_CHECK(rule.may_hold(10635, 0.0f));
  KCD2S_CHECK(rule.may_hold(10635, 0.2f));
  rule.reset();
  KCD2S_CHECK(rule.may_hold(10636, 0.0f));
  KCD2S_CHECK(rule.may_hold(10636, 0.1f));
  KCD2S_CHECK(!rule.may_hold(10635, 0.2f));
}
