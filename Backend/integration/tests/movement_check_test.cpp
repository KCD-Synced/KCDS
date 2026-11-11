// The server's check of a player's movement: impossible steps are refused with a reason.
#include "kcd2s/integration/movement_check.hpp"

#include <string>

#include "kcd2s/testkit.hpp"

using kcd2s::core::Vec3;
using kcd2s::integration::check_movement;
using kcd2s::integration::MovementCheck;
using kcd2s::integration::MovementStrikes;

KCD2S_TEST(movement_check, a_walk_and_a_gallop_pass) {
  MovementCheck check;
  KCD2S_CHECK(check_movement(check, Vec3{0, 0, 0}, 1000, 1, false).ok);
  // 1.5 m in a second on foot.
  KCD2S_CHECK(check_movement(check, Vec3{1.5f, 0, 0}, 2000, 1, false).ok);
  // 18 m in a second on a horse.
  KCD2S_CHECK(check_movement(check, Vec3{19.5f, 0, 0}, 3000, 1, true).ok);
}

KCD2S_TEST(movement_check, a_too_fast_step_has_its_reason) {
  MovementCheck check;
  check_movement(check, Vec3{0, 0, 0}, 1000, 1, false);
  const auto verdict = check_movement(check, Vec3{14.2f, 0, 0}, 2000, 1, false);
  KCD2S_CHECK(!verdict.ok);
  KCD2S_CHECK_EQ(verdict.reason, std::string("speed 14.2 m/s > max 10.0 (on foot)"));
  // Enforced, the player goes back where its last accepted step ended.
  KCD2S_CHECK((verdict.accepted == Vec3{0, 0, 0}));
  // The same jump is reported once: the next step starts where it ended.
  KCD2S_CHECK(check_movement(check, Vec3{15.2f, 0, 0}, 3000, 1, false).ok);
  // Rising faster than a body climbs.
  const auto up = check_movement(check, Vec3{15.2f, 0, 30}, 4000, 1, false);
  KCD2S_CHECK(!up.ok);
  KCD2S_CHECK_EQ(up.reason, std::string("rising 30.0 m/s > max 12.0"));
  // Falling is never refused.
  KCD2S_CHECK(check_movement(check, Vec3{15.2f, 0, -20}, 5000, 1, false).ok);
}

KCD2S_TEST(movement_check, a_teleport_a_pause_and_clock_jitter_start_again) {
  MovementCheck check;
  check_movement(check, Vec3{0, 0, 0}, 1000, 1, false);
  // A server teleport (a new forced-state epoch) starts the check again.
  KCD2S_CHECK(check_movement(check, Vec3{500, 0, 0}, 1500, 2, false).ok);
  // A pause of the owner's clock (a loading screen) too.
  KCD2S_CHECK(check_movement(check, Vec3{900, 0, 0}, 9000, 2, false).ok);
  // Steps closer than the shortest judged time wait for the next ones.
  KCD2S_CHECK(check_movement(check, Vec3{901, 0, 0}, 9050, 2, false).ok);
  KCD2S_CHECK(!check_movement(check, Vec3{905, 0, 0}, 9200, 2, false).ok);
}

KCD2S_TEST(movement_check, enforcement_waits_for_a_third_refused_step_in_ten_seconds) {
  MovementStrikes strikes;
  // Two refused steps far apart never put the player back.
  KCD2S_CHECK(!strikes.strike(0.0));
  KCD2S_CHECK(!strikes.strike(11.0));
  KCD2S_CHECK(!strikes.strike(22.0));
  // Three within the window do, and the count starts again.
  KCD2S_CHECK(!strikes.strike(23.0));
  KCD2S_CHECK(strikes.strike(24.0));
  KCD2S_CHECK(!strikes.strike(25.0));
  KCD2S_CHECK(!strikes.strike(26.0));
  KCD2S_CHECK(strikes.strike(27.0));
}
