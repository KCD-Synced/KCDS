#include "kcd2s/integration/melee_reach.hpp"
#include "kcd2s/testkit.hpp"
using namespace kcd2s;
using namespace kcd2s::integration;

// Lag compensation for melee: a contact counts when the pair stood within reach at some moment of
// the last half second.
namespace {

void walk(PoseHistory& history, double from, double to, core::Vec3 start, core::Vec3 velocity) {
  for (double t = from; t <= to + 1e-9; t += 1.0 / 60) {
    const float dt = float(t - from);
    history.record(
        t, {start.x + velocity.x * dt, start.y + velocity.y * dt, start.z + velocity.z * dt});
  }
}

}  // namespace

KCD2S_TEST(melee_reach, a_victim_that_ran_on_still_counts_where_the_attacker_saw_it) {
  PoseHistory attacker;
  PoseHistory victim;
  // The attacker stands; the victim ran away at 8 m/s and is 6.4 m off now, but was 2.4 m away
  // half a second ago, which the attacker's game still showed.
  walk(attacker, 0.0, 1.0, {0, 0, 0}, {0, 0, 0});
  walk(victim, 0.0, 1.0, {-1.6f, 0, 0}, {8, 0, 0});
  const auto verdict = check_melee_reach(attacker, victim, 1.0);
  KCD2S_CHECK(verdict.ok);
  KCD2S_CHECK(verdict.distance < kMeleeReach);
}

KCD2S_TEST(melee_reach, a_victim_that_was_never_near_is_refused_with_the_distance) {
  PoseHistory attacker;
  PoseHistory victim;
  walk(attacker, 0.0, 1.0, {0, 0, 0}, {0, 0, 0});
  walk(victim, 0.0, 1.0, {12, 0, 0}, {0, 0, 0});
  const auto verdict = check_melee_reach(attacker, victim, 1.0);
  KCD2S_CHECK(!verdict.ok);
  KCD2S_CHECK(verdict.distance > 11.9f && verdict.distance < 12.1f);
  KCD2S_CHECK(verdict.reason.find("distance 12.0 m > reach 5.0 m") == 0);
  // Near only longer ago than the window: refused too.
  PoseHistory left;
  walk(left, 0.0, 0.3, {1, 0, 0}, {0, 0, 0});
  walk(left, 0.3 + 1.0 / 60, 1.0, {20, 0, 0}, {0, 0, 0});
  KCD2S_CHECK(!check_melee_reach(attacker, left, 1.0).ok);
}

KCD2S_TEST(melee_reach, without_samples_nothing_is_judged_and_the_ring_keeps_the_newest) {
  PoseHistory attacker;
  PoseHistory victim;
  KCD2S_CHECK(check_melee_reach(attacker, victim, 1.0).ok);
  walk(attacker, 0.0, 10.0, {0, 0, 0}, {0, 0, 0});
  KCD2S_CHECK_EQ(attacker.size(), PoseHistory::kPoseSamples);
  KCD2S_CHECK(check_melee_reach(attacker, victim, 10.0).ok);
  attacker.clear();
  KCD2S_CHECK_EQ(attacker.size(), std::size_t(0));
}
