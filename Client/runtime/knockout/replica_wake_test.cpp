#include "replica_wake.hpp"

#include "kcd2s/testkit.hpp"

namespace {
using kcds::runtime::knockout::ReplicaWake;
using kcds::runtime::knockout::ReplicaWakeScope;

const ReplicaWakeScope body{1, 2, 100, 200, 300};

void down_up(ReplicaWake& wake) {
  wake.bind(body);
  wake.observe_owner(body, true);
  wake.observe_native(body, {true, true, false});
  wake.observe_owner(body, false);
}
}  // namespace

KCD2S_TEST(replica_wake, native_knockout_and_original_gates_precede_one_reentrant_safe_request) {
  ReplicaWake wake;
  down_up(wake);
  KCD2S_CHECK(wake.pending());
  KCD2S_CHECK(wake.owns_body());
  KCD2S_CHECK(!wake.request(body, true));
  wake.observe_native(body, {false, true, false});
  KCD2S_CHECK(!wake.request(body, false));
  KCD2S_CHECK(wake.request(body, true));
  KCD2S_CHECK(wake.requested());
  KCD2S_CHECK(!wake.request(body, true));
  wake.observe_owner(body, false);
  wake.bind(body);
  KCD2S_CHECK(!wake.request(body, true));
}

KCD2S_TEST(replica_wake, queued_getup_gap_retains_native_ownership_until_observed_exit) {
  ReplicaWake wake;
  down_up(wake);
  wake.observe_native(body, {false, true, false});
  KCD2S_CHECK(wake.request(body, true));
  for (unsigned frame = 0; frame < 10; ++frame) {
    wake.observe_native(body, {false, false, false});
    KCD2S_CHECK(wake.owns_body());
    KCD2S_CHECK(!wake.complete());
    KCD2S_CHECK(!wake.request(body, true));
  }
  wake.observe_native(body, {false, false, true});
  KCD2S_CHECK(wake.owns_body());
  wake.observe_native(body, {false, false, false});
  KCD2S_CHECK(wake.complete());
  KCD2S_CHECK(!wake.owns_body());
}

KCD2S_TEST(replica_wake, native_exit_waits_for_remaining_fall_scope_and_knockout_clear) {
  ReplicaWake wake;
  down_up(wake);
  wake.observe_native(body, {false, true, false});
  KCD2S_CHECK(wake.request(body, true));
  wake.observe_native(body, {false, true, true});
  wake.observe_native(body, {true, true, false});
  KCD2S_CHECK(!wake.complete());
  KCD2S_CHECK(wake.owns_body());
  wake.observe_native(body, {true, false, false});
  KCD2S_CHECK(!wake.complete());
  wake.observe_native(body, {false, false, false});
  KCD2S_CHECK(wake.complete());
}

KCD2S_TEST(replica_wake, no_getup_observation_never_fabricates_completion_before_or_after_request) {
  ReplicaWake wake;
  down_up(wake);
  wake.observe_native(body, {false, false, false});
  KCD2S_CHECK(!wake.request(body, true));
  KCD2S_CHECK(wake.pending());
  KCD2S_CHECK(!wake.complete());
  KCD2S_CHECK(wake.owns_body());
  wake.observe_native(body, {false, true, false});
  KCD2S_CHECK(wake.request(body, true));
  wake.observe_native(body, {false, false, false});
  KCD2S_CHECK(!wake.complete());
  KCD2S_CHECK(wake.owns_body());
}

KCD2S_TEST(replica_wake, already_native_getting_up_does_not_issue_another_wake) {
  ReplicaWake wake;
  down_up(wake);
  wake.observe_native(body, {false, false, true});
  KCD2S_CHECK(!wake.request(body, true));
  KCD2S_CHECK(!wake.requested());
  KCD2S_CHECK(wake.owns_body());
  wake.observe_native(body, {false, false, false});
  KCD2S_CHECK(wake.complete());
}

KCD2S_TEST(replica_wake, getup_edge_before_owner_up_is_retained_even_while_native_knockout_is_set) {
  ReplicaWake wake;
  wake.bind(body);
  wake.observe_owner(body, true);
  KCD2S_CHECK(!wake.owns_body());
  wake.observe_native(body, {true, true, false});
  wake.observe_native(body, {true, false, true});
  wake.observe_owner(body, false);
  KCD2S_CHECK(wake.owns_body());
  KCD2S_CHECK(!wake.request(body, true));
  wake.observe_native(body, {true, false, false});
  KCD2S_CHECK(!wake.complete());
  wake.observe_native(body, {false, false, false});
  KCD2S_CHECK(wake.complete());
}

KCD2S_TEST(replica_wake, changed_revision_epoch_or_body_rejects_stale_work) {
  for (unsigned field = 0; field < 5; ++field) {
    ReplicaWake wake;
    down_up(wake);
    wake.observe_native(body, {false, true, false});
    KCD2S_CHECK(wake.request(body, true));
    auto next = body;
    if (field == 0) {
      ++next.revision;
    } else if (field == 1) {
      ++next.epoch;
    } else if (field == 2) {
      ++next.actor;
    } else if (field == 3) {
      ++next.soul;
    } else {
      ++next.model;
    }
    KCD2S_CHECK(wake.bind(next));
    KCD2S_CHECK(!wake.owns_body());
    KCD2S_CHECK(!wake.observe_owner(body, false));
    KCD2S_CHECK(!wake.observe_native(body, {false, false, true}));
    KCD2S_CHECK(!wake.request(body, true));
    KCD2S_CHECK(!wake.requested());
    KCD2S_CHECK(!wake.complete());
  }
}

KCD2S_TEST(replica_wake, down_again_cancels_old_attempt_and_requires_a_new_up_transition) {
  ReplicaWake wake;
  down_up(wake);
  wake.observe_native(body, {false, true, false});
  KCD2S_CHECK(wake.request(body, true));
  wake.observe_native(body, {false, false, true});
  wake.observe_owner(body, true);
  wake.observe_native(body, {true, true, false});
  KCD2S_CHECK(!wake.requested());
  KCD2S_CHECK(!wake.complete());
  KCD2S_CHECK(!wake.request(body, true));
  wake.observe_owner(body, false);
  wake.observe_native(body, {false, true, false});
  KCD2S_CHECK(wake.request(body, true));
}

KCD2S_TEST(replica_wake, presentation_only_sleep_does_not_wait_for_a_nonexistent_fallen_action) {
  ReplicaWake wake;
  wake.bind(body);
  wake.observe_owner(body, true);
  wake.observe_native(body, {true, false, false});
  wake.observe_owner(body, false);
  KCD2S_CHECK(wake.owns_body());
  wake.observe_native(body, {true, false, false});
  KCD2S_CHECK(wake.owns_body());
  wake.observe_native(body, {false, false, false});
  KCD2S_CHECK(!wake.owns_body());
  KCD2S_CHECK(!wake.complete());
  KCD2S_CHECK(!wake.take_completion(body));
  KCD2S_CHECK(!wake.request(body, true));
  // A late original fallen action is observed, rather than inferred from a profile.
  wake.observe_native(body, {false, true, false});
  KCD2S_CHECK(wake.owns_body());
  KCD2S_CHECK(wake.request(body, true));
}

KCD2S_TEST(replica_wake, same_body_epoch_change_retains_fall_and_rejects_old_observations) {
  ReplicaWake wake;
  wake.bind(body);
  wake.observe_owner(body, true);
  wake.observe_native(body, {true, true, false});
  auto next = body;
  ++next.epoch;
  KCD2S_CHECK(wake.continue_body(next));
  KCD2S_CHECK(!wake.observe_owner(body, false));
  KCD2S_CHECK(!wake.observe_native(body, {false, false, true}));
  wake.observe_owner(next, false);
  wake.observe_native(next, {false, true, false});
  KCD2S_CHECK(wake.request(next, true));
  ++next.epoch;
  KCD2S_CHECK(wake.continue_body(next));
  KCD2S_CHECK(wake.requested());
  KCD2S_CHECK(!wake.request(next, true));
  wake.observe_native(next, {false, false, true});
  wake.observe_native(next, {false, false, false});
  KCD2S_CHECK(wake.take_completion(next));
  KCD2S_CHECK(!wake.take_completion(next));
}

KCD2S_TEST(replica_wake, completion_edge_is_retained_until_consumed_and_rearmed_for_next_fall) {
  ReplicaWake wake;
  down_up(wake);
  wake.observe_native(body, {false, false, true});
  wake.observe_native(body, {false, false, false});
  wake.observe_owner(body, false);
  auto stale = body;
  ++stale.epoch;
  KCD2S_CHECK(!wake.take_completion(stale));
  KCD2S_CHECK(wake.take_completion(body));
  KCD2S_CHECK(!wake.take_completion(body));
  wake.observe_owner(body, true);
  wake.observe_owner(body, false);
  wake.observe_native(body, {false, false, true});
  wake.observe_native(body, {false, false, false});
  KCD2S_CHECK(wake.take_completion(body));
}

KCD2S_TEST(replica_wake, body_continuity_never_transfers_across_world_or_native_identity_changes) {
  for (unsigned field = 0; field < 4; ++field) {
    ReplicaWake wake;
    down_up(wake);
    auto next = body;
    if (field == 0) {
      ++next.revision;
    } else if (field == 1) {
      ++next.actor;
    } else if (field == 2) {
      ++next.soul;
    } else {
      ++next.model;
    }
    KCD2S_CHECK(wake.continue_body(next));
    KCD2S_CHECK(!wake.owns_body());
    KCD2S_CHECK(!wake.observe_owner(body, false));
    wake.observe_owner(next, false);
    wake.observe_native(next, {false, true, false});
    KCD2S_CHECK(!wake.request(next, true));
  }
}

KCD2S_TEST(replica_wake, up_only_binding_cannot_manufacture_a_wake_and_clear_retires_work) {
  ReplicaWake wake;
  wake.bind(body);
  wake.observe_owner(body, false);
  wake.observe_native(body, {false, false, false});
  KCD2S_CHECK(!wake.request(body, true));
  KCD2S_CHECK(!wake.pending());
  KCD2S_CHECK(!wake.owns_body());
  down_up(wake);
  wake.clear();
  KCD2S_CHECK(!wake.owns_body());
  KCD2S_CHECK(!wake.observe_native(body, {false, true, false}));
  KCD2S_CHECK(!wake.request(body, true));
  down_up(wake);
  KCD2S_CHECK(!wake.bind({}));
  KCD2S_CHECK(!wake.owns_body());
}
