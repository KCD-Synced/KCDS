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

KCD2S_TEST(movement_check, a_fast_travel_carries_the_player_along_its_way_only) {
  using kcd2s::integration::FastTravel;
  using kcd2s::integration::on_fast_travel;
  // From near Kuttenberg's stables towards the town, about 3.3 km straight.
  const FastTravel travel{true, 100.0, Vec3{825, 3364, 141}, Vec3{2968, 908, 66}};
  // A refused step reports where it ended.
  MovementCheck check;
  check_movement(check, Vec3{825, 3364, 141}, 1000, 1, false);
  const auto step = check_movement(check, Vec3{900, 3300, 140}, 2000, 1, false);
  KCD2S_CHECK(!step.ok);
  KCD2S_CHECK(step.to.x == 900.0f && step.to.y == 3300.0f);
  // On the way, even off the straight line where the road winds, and at the end.
  KCD2S_CHECK(on_fast_travel(travel, step.to, 101.0));
  KCD2S_CHECK(on_fast_travel(travel, Vec3{2200, 2400, 100}, 130.0));
  KCD2S_CHECK(on_fast_travel(travel, Vec3{2968, 908, 66}, 160.0));
  // Not far off it, not after the travel's time, not without a travel.
  KCD2S_CHECK(!on_fast_travel(travel, Vec3{100, 100, 50}, 130.0));
  KCD2S_CHECK(!on_fast_travel(travel, Vec3{2968, 908, 66}, 100.0 + 181.0));
  KCD2S_CHECK(!on_fast_travel(FastTravel{}, Vec3{900, 3300, 140}, 101.0));
}

KCD2S_TEST(movement_check, a_cart_boarding_by_teleport_lets_the_jump_to_the_cart_pass) {
  using kcd2s::integration::Boarding;
  using kcd2s::integration::on_boarding;
  // Ordered at 50 s for a cart standing at the mill's bridge.
  Boarding boarding{true, 50.0, Vec3{1106, 1345, 11}};
  // The jump to the seat's climb start, a few metres from the cart, within the time.
  KCD2S_CHECK(on_boarding(boarding, Vec3{1104.3, 1348.3, 11}, 50.2));
  KCD2S_CHECK(on_boarding(boarding, Vec3{1110, 1344, 12}, 53.0));
  // Not far from the cart, not without a boarding.
  KCD2S_CHECK(!on_boarding(boarding, Vec3{1130, 1345, 11}, 51.0));
  Boarding none;
  KCD2S_CHECK(!on_boarding(none, Vec3{1106, 1345, 11}, 51.0));
  // After its time it is over, also near the cart later.
  KCD2S_CHECK(!on_boarding(boarding, Vec3{1106, 1345, 11}, 57.0));
  KCD2S_CHECK(!boarding.active);
  KCD2S_CHECK(!on_boarding(boarding, Vec3{1106, 1345, 11}, 57.5));
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

KCD2S_TEST(movement_check, a_noclip_flight_passes_only_for_a_player_who_may_fly) {
  using kcd2s::integration::excused_noclip_flight;
  using kcd2s::integration::MovementRefusal;
  MovementCheck check;
  KCD2S_CHECK(check_movement(check, Vec3{0, 0, 0}, 0, 0, false).ok);
  // 50 m/s on foot: refused, as a flight across a village is.
  MovementRefusal refusal{1, check_movement(check, Vec3{50, 0, 0}, 1000, 0, false), true};
  KCD2S_CHECK(!refusal.verdict.ok);
  KCD2S_CHECK(excused_noclip_flight(refusal, true));
  // A player a resource forbade NoClip: the report does not excuse the step.
  KCD2S_CHECK(!excused_noclip_flight(refusal, false));
  // Not flying: the step counts as before.
  refusal.no_clip = false;
  KCD2S_CHECK(!excused_noclip_flight(refusal, true));
}
