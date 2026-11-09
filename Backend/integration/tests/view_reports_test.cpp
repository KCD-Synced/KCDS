// Resource host view reports as changed-state deltas: the tracker sends only entity
// records that changed and per-peer view changes, never the whole view of every peer.
#include "../local/resources/report_cache.hpp"
#include "../local/resources/view_reports.hpp"

#include <vector>

#include "kcd2s/testkit.hpp"

using kcd2s::core::EntityId;
using kcd2s::local::resources::PeerView;
using kcd2s::local::resources::PeerViewDelta;
using kcd2s::local::resources::ReportCache;
using kcd2s::local::resources::ReportedEntity;
using kcd2s::local::resources::ViewMember;
using kcd2s::local::resources::ViewReportTracker;

namespace {

ReportedEntity entity(EntityId id, float x = 0.0f) {
  ReportedEntity record;
  record.script_id = id;
  record.native_id = id;
  record.kind = "player";
  record.dimension = 0xffffffffu;
  record.position = {x, 2.0f, 3.0f};
  return record;
}

PeerView view(kcd2s::core::PeerId peer, EntityId local, std::vector<ViewMember> members) {
  PeerView result;
  result.peer = peer;
  result.local_player = local;
  result.members = std::move(members);
  return result;
}

}  // namespace

KCD2S_TEST(view_reports, first_view_adds_every_member_and_lists_owned_entities) {
  ViewReportTracker tracker;
  tracker.begin_cycle();
  KCD2S_CHECK(tracker.refresh(entity(1)));
  KCD2S_CHECK(tracker.refresh(entity(2)));
  PeerViewDelta delta;
  KCD2S_CHECK(tracker.update_view(view(7, 1, {{2, false}, {1, true}}), delta));
  KCD2S_CHECK_EQ(delta.peer, 7u);
  KCD2S_CHECK_EQ(delta.local_player, 1u);
  KCD2S_CHECK(delta.added == std::vector<EntityId>({1, 2}));
  KCD2S_CHECK(delta.removed.empty());
  KCD2S_CHECK(delta.owned == std::vector<EntityId>({1}));
  KCD2S_CHECK_EQ(tracker.references(1), std::size_t{1});
}

KCD2S_TEST(view_reports, unchanged_views_and_records_send_nothing) {
  ViewReportTracker tracker;
  tracker.begin_cycle();
  tracker.refresh(entity(1));
  PeerViewDelta delta;
  tracker.update_view(view(1, 1, {{1, true}}), delta);
  tracker.begin_cycle();
  KCD2S_CHECK(tracker.needs_refresh(1));
  KCD2S_CHECK(!tracker.refresh(entity(1)));
  KCD2S_CHECK(!tracker.update_view(view(1, 1, {{1, true}}), delta));
}

KCD2S_TEST(view_reports, a_moving_entity_is_reported_once_per_cycle) {
  ViewReportTracker tracker;
  tracker.begin_cycle();
  tracker.refresh(entity(1));
  PeerViewDelta delta;
  tracker.update_view(view(1, 1, {{1, true}}), delta);
  tracker.update_view(view(2, 2, {{1, false}}), delta);
  tracker.begin_cycle();
  KCD2S_CHECK(tracker.needs_refresh(1));
  KCD2S_CHECK(tracker.refresh(entity(1, 5.0f)));
  // A second peer of the same cycle sees the entity: it is not read or sent again.
  KCD2S_CHECK(!tracker.needs_refresh(1));
  tracker.begin_cycle();
  KCD2S_CHECK(tracker.needs_refresh(1));
  KCD2S_CHECK(!tracker.refresh(entity(1, 5.0f)));
}

// A crowd's records come in the slice of their id, not all in the cycle's first slice.
KCD2S_TEST(view_reports, known_records_are_due_in_the_slice_of_their_id) {
  ViewReportTracker tracker;
  tracker.begin_cycle();
  std::vector<ViewMember> members;
  for (EntityId id = 1; id <= 12; ++id) {
    tracker.refresh(entity(id));
    members.push_back({id, id == 1});
  }
  PeerViewDelta delta;
  tracker.update_view(view(1, 1, members), delta);
  tracker.begin_cycle();
  KCD2S_CHECK(tracker.due_in_slice(6, 0) == std::vector<EntityId>({6, 12}));
  KCD2S_CHECK(tracker.due_in_slice(6, 1) == std::vector<EntityId>({1, 7}));
  tracker.refresh(entity(7, 1.0f));
  KCD2S_CHECK(tracker.due_in_slice(6, 1) == std::vector<EntityId>({1}));
}

KCD2S_TEST(view_reports, leaving_members_are_removed_and_unreferenced_records_dropped) {
  ViewReportTracker tracker;
  tracker.begin_cycle();
  tracker.refresh(entity(1));
  tracker.refresh(entity(2));
  tracker.refresh(entity(3));
  PeerViewDelta delta;
  tracker.update_view(view(1, 1, {{1, true}, {2, false}, {3, false}}), delta);
  tracker.update_view(view(2, 2, {{2, true}, {3, false}}), delta);

  tracker.begin_cycle();
  KCD2S_CHECK(tracker.update_view(view(1, 1, {{1, true}, {2, false}}), delta));
  KCD2S_CHECK(delta.added.empty());
  KCD2S_CHECK(delta.removed == std::vector<EntityId>({3}));
  // Entity 3 is still in peer 2's view, so its record stays.
  KCD2S_CHECK(tracker.begin_cycle().empty());
  KCD2S_CHECK(tracker.update_view(view(2, 2, {{2, true}}), delta));
  KCD2S_CHECK(delta.removed == std::vector<EntityId>({3}));
  // No view references entity 3 any more: the next cycle drops its record first.
  KCD2S_CHECK(tracker.begin_cycle() == std::vector<EntityId>({3}));
  KCD2S_CHECK(!tracker.known(3));
  KCD2S_CHECK(tracker.known(2));
}

KCD2S_TEST(view_reports, an_ownership_change_alone_is_reported) {
  ViewReportTracker tracker;
  tracker.begin_cycle();
  tracker.refresh(entity(1));
  tracker.refresh(entity(9));
  PeerViewDelta delta;
  tracker.update_view(view(1, 1, {{1, true}, {9, false}}), delta);
  KCD2S_CHECK(tracker.update_view(view(1, 1, {{1, true}, {9, true}}), delta));
  KCD2S_CHECK(delta.added.empty());
  KCD2S_CHECK(delta.removed.empty());
  KCD2S_CHECK(delta.owned == std::vector<EntityId>({1, 9}));
  KCD2S_CHECK(tracker.update_view(view(1, 1, {{1, true}, {9, false}}), delta));
  KCD2S_CHECK(delta.owned == std::vector<EntityId>({1}));
}

KCD2S_TEST(view_reports, departed_peers_release_their_references) {
  ViewReportTracker tracker;
  tracker.begin_cycle();
  tracker.refresh(entity(1));
  tracker.refresh(entity(2));
  PeerViewDelta delta;
  tracker.update_view(view(1, 1, {{1, true}, {2, false}}), delta);
  tracker.update_view(view(2, 2, {{2, true}}), delta);
  tracker.forget_peer(1);
  KCD2S_CHECK_EQ(tracker.tracked_peers(), std::size_t{1});
  KCD2S_CHECK(tracker.begin_cycle() == std::vector<EntityId>({1}));
  // A reconnecting peer id starts from an empty view again.
  tracker.refresh(entity(1));
  KCD2S_CHECK(tracker.update_view(view(1, 1, {{1, true}}), delta));
  KCD2S_CHECK(delta.added == std::vector<EntityId>({1}));
}

KCD2S_TEST(view_reports, reset_reports_everything_to_a_new_resource_host) {
  ViewReportTracker tracker;
  tracker.begin_cycle();
  tracker.refresh(entity(1));
  PeerViewDelta delta;
  tracker.update_view(view(1, 1, {{1, true}}), delta);
  tracker.reset();
  KCD2S_CHECK_EQ(tracker.tracked_entities(), std::size_t{0});
  tracker.begin_cycle();
  KCD2S_CHECK(tracker.refresh(entity(1)));
  KCD2S_CHECK(tracker.update_view(view(1, 1, {{1, true}}), delta));
}

KCD2S_TEST(view_reports, a_dense_idle_gathering_produces_no_reports) {
  // 300 peers that all see each other: after the first cycle nothing changes and nothing is sent.
  constexpr EntityId kPeers = 300;
  ViewReportTracker tracker;
  std::vector<ViewMember> everyone;
  for (EntityId id = 1; id <= kPeers; ++id) {
    everyone.push_back({id, false});
  }
  for (int cycle = 0; cycle < 3; ++cycle) {
    tracker.begin_cycle();
    std::size_t upserts = 0;
    std::size_t deltas = 0;
    for (EntityId peer = 1; peer <= kPeers; ++peer) {
      std::vector<ViewMember> members = everyone;
      members[peer - 1].owned = true;
      for (const ViewMember& member : members) {
        if (tracker.needs_refresh(member.entity) && tracker.refresh(entity(member.entity))) {
          upserts += 1;
        }
      }
      PeerViewDelta delta;
      if (tracker.update_view(view(peer, peer, std::move(members)), delta)) {
        deltas += 1;
      }
    }
    if (cycle == 0) {
      KCD2S_CHECK_EQ(upserts, std::size_t{kPeers});
      KCD2S_CHECK_EQ(deltas, std::size_t{kPeers});
    } else {
      KCD2S_CHECK_EQ(upserts, std::size_t{0});
      KCD2S_CHECK_EQ(deltas, std::size_t{0});
    }
  }
}

KCD2S_TEST(report_cache, reports_only_changes_and_again_after_forget) {
  ReportCache<int, int> cache;
  KCD2S_CHECK(cache.changed(1, 10));
  KCD2S_CHECK(!cache.changed(1, 10));
  KCD2S_CHECK(cache.changed(1, 11));
  cache.forget(1);
  KCD2S_CHECK(cache.changed(1, 11));
  KCD2S_CHECK(cache.changed(2, 11));
  KCD2S_CHECK_EQ(cache.size(), std::size_t{2});
  cache.clear();
  KCD2S_CHECK(cache.changed(1, 11));
}
