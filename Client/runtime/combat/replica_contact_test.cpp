#include "replica_contact.hpp"
#include "kcd2s/testkit.hpp"

using kcds::runtime::combat::ReplicaContact;
using kcds::runtime::combat::ReplicaContactScope;

namespace {
constexpr ReplicaContactScope body{1, 2, 100, 200};
}

KCD2S_TEST(replica_contact, exact_active_swing_is_consumed_once) {
  ReplicaContact contact;
  KCD2S_CHECK(contact.announce(body, 10, 3));
  KCD2S_CHECK(!contact.consume(body, 10, 300, 3));
  KCD2S_CHECK(contact.start(body, 300, 3));
  KCD2S_CHECK(!contact.consume(body, 9, 300, 3));
  KCD2S_CHECK(!contact.consume(body, 10, 301, 3));
  KCD2S_CHECK(!contact.consume(body, 10, 300, 4));
  KCD2S_CHECK(contact.consume(body, 10, 300, 3));
  KCD2S_CHECK(!contact.consume(body, 10, 300, 3));
}

KCD2S_TEST(replica_contact, exit_and_pointer_reuse_cannot_reactivate_old_contact) {
  ReplicaContact contact;
  KCD2S_CHECK(contact.announce(body, 10, 3));
  KCD2S_CHECK(contact.start(body, 300, 3));
  contact.exit(300);
  KCD2S_CHECK(!contact.start(body, 300, 3));
  KCD2S_CHECK(!contact.consume(body, 10, 300, 3));
  KCD2S_CHECK(!contact.announce(body, 10, 3));
  KCD2S_CHECK(contact.announce(body, 11, 3));
  KCD2S_CHECK(contact.start(body, 300, 3));
  KCD2S_CHECK(!contact.consume(body, 10, 300, 3));
  KCD2S_CHECK(contact.consume(body, 11, 300, 3));
}

KCD2S_TEST(replica_contact, newer_marker_retires_old_action_before_replacement_start) {
  ReplicaContact contact;
  KCD2S_CHECK(contact.announce(body, 10, 3));
  KCD2S_CHECK(contact.start(body, 300, 3));
  KCD2S_CHECK(contact.announce(body, 11, 4));
  contact.exit(300);
  KCD2S_CHECK(!contact.consume(body, 10, 300, 3));
  KCD2S_CHECK(contact.start(body, 400, 4));
  contact.exit(300);
  KCD2S_CHECK(contact.consume(body, 11, 400, 4));
}

KCD2S_TEST(replica_contact, world_epoch_and_body_replacement_invalidate_binding) {
  for (unsigned field = 0; field < 4; ++field) {
    ReplicaContact contact;
    KCD2S_CHECK(contact.announce(body, 10, 3));
    KCD2S_CHECK(contact.start(body, 300, 3));
    auto replacement = body;
    if (field == 0) {
      ++replacement.revision;
    } else if (field == 1) {
      ++replacement.epoch;
    } else if (field == 2) {
      ++replacement.actor;
    } else {
      ++replacement.movement;
    }
    KCD2S_CHECK(!contact.consume(replacement, 10, 300, 3));
    KCD2S_CHECK(contact.announce(replacement, 11, 3));
    KCD2S_CHECK(!contact.consume(replacement, 10, 300, 3));
    KCD2S_CHECK(contact.start(replacement, 400, 3));
    KCD2S_CHECK(contact.consume(replacement, 11, 400, 3));
  }
}

KCD2S_TEST(replica_contact, start_before_marker_and_wrong_native_kind_fail_closed) {
  ReplicaContact contact;
  KCD2S_CHECK(!contact.start(body, 300, 3));
  KCD2S_CHECK(!contact.announce(body, 0, 3));
  KCD2S_CHECK(!contact.announce(body, 1, 7));
  KCD2S_CHECK(contact.announce(body, 10, 3));
  KCD2S_CHECK(!contact.start(body, 300, 4));
  KCD2S_CHECK(!contact.consume(body, 10, 300, 3));
}

KCD2S_TEST(replica_contact, refused_input_cannot_bind_a_later_swing_without_its_marker) {
  ReplicaContact contact;
  KCD2S_CHECK(contact.announce(body, 10, 3));
  contact.finish_input();
  KCD2S_CHECK(!contact.start(body, 400, 3));
  KCD2S_CHECK(!contact.consume(body, 10, 400, 3));
  KCD2S_CHECK(contact.announce(body, 11, 3));
  KCD2S_CHECK(contact.start(body, 400, 3));
  contact.finish_input();
  KCD2S_CHECK(contact.consume(body, 11, 400, 3));
}

KCD2S_TEST(replica_contact, factory_action_can_start_after_the_original_input_returns) {
  ReplicaContact contact;
  KCD2S_CHECK(contact.announce(body, 10, 3));
  KCD2S_CHECK(contact.construct(body, 300, 3));
  contact.finish_input();
  KCD2S_CHECK(!contact.consume(body, 10, 300, 3));
  KCD2S_CHECK(!contact.start(body, 400, 3, false));
  KCD2S_CHECK(contact.start(body, 300, 3, false));
  KCD2S_CHECK(contact.consume(body, 10, 300, 3));
}

KCD2S_TEST(replica_contact, late_start_requires_the_factory_action_not_an_unused_marker) {
  ReplicaContact contact;
  KCD2S_CHECK(contact.announce(body, 10, 3));
  KCD2S_CHECK(!contact.start(body, 300, 3, false));
  contact.finish_input();
  KCD2S_CHECK(!contact.start(body, 300, 3, false));
  KCD2S_CHECK(!contact.consume(body, 10, 300, 3));
}
