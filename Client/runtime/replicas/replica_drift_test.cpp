#include "replica_drift.hpp"

#include "kcd2s/testkit.hpp"

using kcds::runtime::replicas::ReplicaDrift;

KCD2S_TEST(replica_drift, a_body_near_its_position_is_left_to_its_movement) {
  ReplicaDrift drift;
  KCD2S_CHECK(!drift.should_place(0.5f, 0.5f, 0.2f, 0.0));
  KCD2S_CHECK(!drift.should_place(0.5f, 0.5f, 0.2f, 10.0));
}

KCD2S_TEST(replica_drift, a_body_stuck_a_few_metres_away_is_placed_after_half_a_second) {
  ReplicaDrift drift;
  KCD2S_CHECK(!drift.should_place(2.0f, 0.0f, 0.0f, 1.0));
  KCD2S_CHECK(!drift.should_place(2.0f, 0.0f, 0.0f, 1.4));
  KCD2S_CHECK(drift.should_place(2.0f, 0.0f, 0.0f, 1.5));
  // Placed: the next drift waits its own half second again.
  KCD2S_CHECK(!drift.should_place(2.0f, 0.0f, 0.0f, 1.6));
}

KCD2S_TEST(replica_drift, coming_near_again_restarts_the_wait) {
  ReplicaDrift drift;
  KCD2S_CHECK(!drift.should_place(2.0f, 0.0f, 0.0f, 1.0));
  KCD2S_CHECK(!drift.should_place(0.2f, 0.0f, 0.0f, 1.3));
  KCD2S_CHECK(!drift.should_place(2.0f, 0.0f, 0.0f, 1.6));
  KCD2S_CHECK(drift.should_place(2.0f, 0.0f, 0.0f, 2.1));
}

KCD2S_TEST(replica_drift, a_body_a_floor_apart_is_placed_at_once) {
  ReplicaDrift drift;
  KCD2S_CHECK(drift.should_place(0.3f, 0.0f, 2.5f, 0.0));
  KCD2S_CHECK(drift.should_place(0.0f, 0.0f, -1.6f, 0.1));
}
