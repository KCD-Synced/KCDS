#include "kcd2s/integration/movement_mode.hpp"
#include "kcd2s/testkit.hpp"
using namespace kcd2s::integration;

// The pace rule a server puts on a player: MOVEMENT_MODE.
KCD2S_TEST(movement_mode, round_trip) {
  for (bool by_default : {false, true}) {
    for (bool enforced : {false, true}) {
      MovementMode mode;
      mode.walk_by_default = by_default;
      mode.walk_enforced = enforced;
      MovementMode decoded;
      decoded.walk_by_default = !by_default;
      KCD2S_CHECK(decode_movement_mode(encode_movement_mode(mode), decoded) && decoded == mode);
    }
  }
}

KCD2S_TEST(movement_mode, malformed_refused) {
  MovementMode mode;
  mode.walk_enforced = true;
  KCD2S_CHECK(!decode_movement_mode({kMovementModeTag}, mode));
  KCD2S_CHECK(!decode_movement_mode({kMovementModeTag, 4}, mode));
  KCD2S_CHECK(!decode_movement_mode({kMovementModeTag, 1, 0}, mode));
  KCD2S_CHECK(!decode_movement_mode({kMovementModeTag + 1, 1}, mode));
  // A refused message leaves the rule alone.
  KCD2S_CHECK(mode.walk_enforced && !mode.walk_by_default);
}
