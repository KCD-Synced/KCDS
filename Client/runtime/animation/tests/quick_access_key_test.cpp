#include "../quick_access_key.hpp"

#include "kcd2s/testkit.hpp"

namespace key = kcds::runtime::animation::quick_access_key;

KCD2S_TEST(quick_access_key, kept_off_while_the_player_may_point_and_switched_off_again_in_time) {
  key::State state;
  KCD2S_CHECK(key::step(state, true, 1000) == key::Switch::Off);
  // Within the recheck time nothing is switched.
  KCD2S_CHECK(key::step(state, true, 1100) == key::Switch::None);
  // The game may have turned it on again meanwhile: off again.
  KCD2S_CHECK(key::step(state, true, 1000 + key::kRecheckMs) == key::Switch::Off);
}

KCD2S_TEST(quick_access_key, given_back_once_when_pointing_is_refused_and_then_left_alone) {
  key::State state;
  // Never turned off: nothing to give back.
  KCD2S_CHECK(key::step(state, false, 0) == key::Switch::None);
  key::step(state, true, 10);
  KCD2S_CHECK(key::step(state, false, 20) == key::Switch::On);
  KCD2S_CHECK(key::step(state, false, 5000) == key::Switch::None);
}
