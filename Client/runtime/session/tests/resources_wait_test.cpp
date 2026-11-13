#include "../resources_wait.hpp"

#include "kcd2s/testkit.hpp"

using kcds::game_client::ResourcesWait;

KCD2S_TEST(resources_wait, a_world_waits_from_its_first_request) {
  ResourcesWait wait;
  KCD2S_CHECK(wait.begin(1, 1, 100.0));
  KCD2S_CHECK(!wait.begin(1, 1, 150.0));
  KCD2S_CHECK(wait.waited(150.0) == 50.0);
}

KCD2S_TEST(resources_wait, a_rejoin_with_the_same_revision_waits_again) {
  ResourcesWait wait;
  KCD2S_CHECK(wait.begin(1, 1, 100.0));
  KCD2S_CHECK(wait.report_once());
  // The same world revision from a new client long after the first join.
  KCD2S_CHECK(wait.begin(1, 2, 400.0));
  KCD2S_CHECK(wait.waited(400.0) == 0.0);
  KCD2S_CHECK(wait.waited(430.0) == 30.0);
  // Its own load without the resources is logged again.
  KCD2S_CHECK(wait.report_once());
  KCD2S_CHECK(!wait.report_once());
}

KCD2S_TEST(resources_wait, a_new_revision_waits_again) {
  ResourcesWait wait;
  KCD2S_CHECK(wait.begin(1, 1, 100.0));
  KCD2S_CHECK(wait.begin(2, 1, 300.0));
  KCD2S_CHECK(wait.waited(300.0) == 0.0);
}
