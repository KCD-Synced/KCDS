#include "needs.hpp"

#include "kcd2s/testkit.hpp"

namespace {

namespace needs = kcds::runtime::needs;

// Leaves the world and comes back, so each test starts with the needs on and nothing kept.
void fresh_world() {
  needs::install(nullptr, nullptr);
  needs::tick(true, true);
  needs::tick(false, true);
  needs::tick(true, true);
}

}  // namespace

KCD2S_TEST(needs_fill, an_awake_body_takes_the_server_fill_and_keeps_nothing) {
  fresh_world();
  needs::set_enabled(false, true);
  KCD2S_CHECK(!needs::tick(true, true));
  KCD2S_CHECK(!needs::tick(true, true));
}

KCD2S_TEST(needs_fill, a_body_that_is_not_awake_is_filled_once_when_it_is) {
  fresh_world();
  needs::set_enabled(false, false);
  KCD2S_CHECK(!needs::tick(true, false));
  KCD2S_CHECK(!needs::tick(true, false));
  KCD2S_CHECK(needs::tick(true, true));
  KCD2S_CHECK(!needs::tick(true, true));
  KCD2S_CHECK(!needs::tick(true, false));
  KCD2S_CHECK(!needs::tick(true, true));
}

KCD2S_TEST(needs_fill, needs_on_again_before_the_wake_drop_the_kept_fill) {
  fresh_world();
  needs::set_enabled(false, false);
  needs::set_enabled(true, false);
  KCD2S_CHECK(!needs::tick(true, true));
}

KCD2S_TEST(needs_fill, leaving_the_world_drops_the_kept_fill) {
  fresh_world();
  needs::set_enabled(false, false);
  KCD2S_CHECK(!needs::tick(false, false));
  KCD2S_CHECK(!needs::tick(true, true));
}
