#include "kcd2s/integration/posture.hpp"
#include "kcd2s/testkit.hpp"
using namespace kcd2s::integration;

// How a player's body stands: the NetworkPlayer field "posture".
KCD2S_TEST(posture, round_trip) {
  for (bool in_air : {false, true}) {
    for (bool crouched : {false, true}) {
      for (int profile : {0, 1, 2, 6, 255}) {
        for (bool guard : {false, true}) {
          PlayerPosture posture;
          posture.in_air = in_air;
          posture.crouched = crouched;
          posture.guard = guard;
          posture.raised = !guard;
          posture.physics_profile = std::uint8_t(profile);
          for (int zone : {-1, 0, 5, 15, 254}) {
            posture.combat_zone = std::int16_t(zone);
            for (bool on_ladder : {false, true}) {
              posture.on_ladder = on_ladder;
              posture.ladder_progress = on_ladder ? std::uint16_t(40000) : std::uint16_t(0);
              for (bool fainted : {false, true}) {
                posture.fainted = fainted;
                PlayerPosture decoded;
                KCD2S_CHECK(parse_posture_field(posture_field(posture), decoded) &&
                            decoded == posture);
              }
            }
          }
        }
      }
    }
  }
}

KCD2S_TEST(posture, unreported_and_malformed_refused) {
  PlayerPosture posture;
  posture.in_air = true;
  posture.physics_profile = 2;
  // A player whose client has not reported (the field's zero) has the defaults.
  KCD2S_CHECK(!parse_posture_field(0, posture));
  KCD2S_CHECK(!parse_posture_field(kPostureInAir | kPostureCrouched, posture));
  KCD2S_CHECK(!parse_posture_field(kPostureReported | 128, posture));
  KCD2S_CHECK(!parse_posture_field(kPostureReported | 0x1000000, posture));
  KCD2S_CHECK(!parse_posture_field(-1, posture));
  // A climb's progress belongs to a body on a ladder only.
  KCD2S_CHECK(!parse_posture_field(kPostureReported | (std::int64_t(1) << 32), posture));
  // A refused field leaves the posture alone.
  KCD2S_CHECK(posture.in_air && !posture.crouched && posture.physics_profile == 2);
  PlayerPosture defaults;
  KCD2S_CHECK(defaults.physics_profile == kNoPhysicsProfile && !defaults.in_air);
}

KCD2S_TEST(posture, ladder_progress_of_a_climb_clip) {
  KCD2S_CHECK(ladder_progress_of(1.0f) == 65535);
  KCD2S_CHECK(ladder_progress_of(2.0f) == 65535);
  KCD2S_CHECK(ladder_time_of(ladder_progress_of(1.0f)) == 1.0f);
  const float half = ladder_time_of(ladder_progress_of(0.5f));
  KCD2S_CHECK(half > 0.4999f && half < 0.5001f);
  // The ladder action rests a climber at 0 or 0.5 of the clip (0.000 after a short climb).
  for (float time : {0.0f, 0.25f, 0.5f, 0.75f, 0.99999f}) {
    const std::uint16_t progress = ladder_progress_of(time);
    KCD2S_CHECK(ladder_clip_held(progress));
    const float decoded = ladder_time_of(progress);
    KCD2S_CHECK(decoded > time - 0.0001f && decoded < time + 0.0001f);
  }
}

// A climber resting at the clip's start is held there; an owner getting on or off the ladder plays
// its base layer on its game's own clock, and its replica is not held at all.
KCD2S_TEST(posture, ladder_rest_at_the_start_differs_from_no_held_clip) {
  KCD2S_CHECK(ladder_progress_of(0.0f) == 1);
  KCD2S_CHECK(ladder_progress_of(-0.5f) == 1);
  KCD2S_CHECK(ladder_time_of(ladder_progress_of(0.0f)) == 0.0f);
  KCD2S_CHECK(!ladder_clip_held(0));
  KCD2S_CHECK(ladder_time_of(0) == 0.0f);
  PlayerPosture resting;
  resting.on_ladder = true;
  resting.ladder_progress = ladder_progress_of(0.0f);
  PlayerPosture getting_on;
  getting_on.on_ladder = true;
  PlayerPosture decoded;
  KCD2S_CHECK(parse_posture_field(posture_field(resting), decoded));
  KCD2S_CHECK(decoded.on_ladder && ladder_clip_held(decoded.ladder_progress));
  KCD2S_CHECK(parse_posture_field(posture_field(getting_on), decoded));
  KCD2S_CHECK(decoded.on_ladder && !ladder_clip_held(decoded.ladder_progress));
}

// A player in the game's exhaustion faint: bit 6, set only while it lies fainted, and
// independent of how its body otherwise stands.
static_assert(kPostureFainted == 64, "the faint is bit 6 of the posture field");

KCD2S_TEST(posture, fainted_bit) {
  PlayerPosture fainted;
  fainted.fainted = true;
  fainted.physics_profile = 3;
  const std::int64_t field = posture_field(fainted);
  KCD2S_CHECK((field & kPostureFainted) == kPostureFainted);

  PlayerPosture awake = fainted;
  awake.fainted = false;
  KCD2S_CHECK((posture_field(awake) & kPostureFainted) == 0);
  KCD2S_CHECK(posture_field(awake) == (field & ~kPostureFainted));

  PlayerPosture decoded;
  KCD2S_CHECK(parse_posture_field(field, decoded) && decoded.fainted &&
              decoded.physics_profile == 3 && !decoded.in_air && !decoded.on_ladder);
  KCD2S_CHECK(parse_posture_field(posture_field(awake), decoded) && !decoded.fainted);
  // A fainted posture differs from the same posture awake, so the change is published.
  KCD2S_CHECK(!(fainted == awake));
  // A player that has not reported is not fainted.
  KCD2S_CHECK(!parse_posture_field(kPostureFainted, decoded));
  PlayerPosture defaults;
  KCD2S_CHECK(!defaults.fainted);
}

KCD2S_TEST(posture, stance_round_trips_and_an_older_report_has_none) {
  using namespace kcd2s::integration;
  for (const int stance : {0, 1, 7, 200, 254, int(kNoStance)}) {
    PlayerPosture posture;
    posture.crouched = true;
    posture.stance = std::uint8_t(stance);
    PlayerPosture parsed;
    KCD2S_CHECK(parse_posture_field(posture_field(posture), parsed));
    KCD2S_CHECK(parsed == posture);
    KCD2S_CHECK_EQ(int(parsed.stance), stance);
  }
  // A client from before the stance sends none of its bits: no stance.
  PlayerPosture old;
  old.crouched = true;
  const std::int64_t field = posture_field(old) & ~kPostureStance;
  PlayerPosture parsed;
  KCD2S_CHECK(parse_posture_field(field, parsed));
  KCD2S_CHECK_EQ(int(parsed.stance), int(kNoStance));
}
