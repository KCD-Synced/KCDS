#include "kcd2s/integration/server.hpp"
#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/pillory.hpp"
#include "kcd2s/testkit.hpp"
using namespace kcd2s;
using namespace kcd2s::integration;

namespace {

struct Link {
  replication::SchemaRegistry schema;
  Server server;
  std::pair<std::unique_ptr<transport::SimLink>, std::unique_ptr<transport::SimLink>> links;
  core::EntityId player = 0;
  core::PeerId peer = 0;
  ClientPeer client;
  double now = 0;

  static replication::SchemaRegistry make_schema() {
    replication::SchemaRegistry schema;
    schema.define("Player").field("position", replication::FieldKind::Vec3,
                                  replication::Priority::High, replication::Authority::Owner);
    return schema;
  }

  Link()
      : schema(make_schema()),
        server(schema),
        links(transport::make_loopback_pair()),
        client(schema, links.second.get()) {
    server.world().set_tick(1);
    player = server.world().spawn(core::type_id("Player"));
    server.world().write(player, 0, core::Vec3{});
    peer = server.add_client(links.first.get(), player, 10000);
    WorldConfig config;
    server.configure_world(peer, config);
    server.set_owner(player, peer);
    run(8);
  }

  void run(int ticks) {
    for (int i = 0; i < ticks; ++i) {
      now += 1.0 / 60;
      server.world().set_tick(server.world().tick() + 1);
      server.tick(now);
      client.tick(now);
    }
  }
};

}  // namespace

KCD2S_TEST(pillory, orders_round_trip_through_the_codec) {
  PilloryOrder put_in;
  put_in.kind = kPilloryPutIn;
  put_in.holder = "Pillory_Rattay 01";
  PilloryOrder decoded;
  KCD2S_CHECK(decode_pillory(encode_pillory(put_in), decoded));
  KCD2S_CHECK(decoded == put_in);

  PilloryOrder release;
  release.kind = kPilloryRelease;
  KCD2S_CHECK(decode_pillory(encode_pillory(release), decoded));
  KCD2S_CHECK(decoded == release);

  PilloryOrder nearest;
  nearest.kind = kPilloryPutInNearest;
  KCD2S_CHECK(decode_pillory(encode_pillory(nearest), decoded));
  KCD2S_CHECK(decoded == nearest);
  nearest.holder = "so_pillory";
  KCD2S_CHECK(encode_pillory(nearest).empty());

  // A put-in needs a holder name, a release has none, the name is printable and bounded.
  PilloryOrder bad = put_in;
  bad.holder.clear();
  KCD2S_CHECK(encode_pillory(bad).empty());
  bad = release;
  bad.holder = "so_pillory";
  KCD2S_CHECK(encode_pillory(bad).empty());
  bad = put_in;
  bad.holder = std::string(kPilloryNameMax + 1, 'a');
  KCD2S_CHECK(encode_pillory(bad).empty());
  bad.holder = std::string(kPilloryNameMax, 'a');
  KCD2S_CHECK(!encode_pillory(bad).empty());
  bad.holder = "tab\there";
  KCD2S_CHECK(encode_pillory(bad).empty());
  bad = put_in;
  bad.kind = 4;
  KCD2S_CHECK(encode_pillory(bad).empty());

  KCD2S_CHECK(!decode_pillory({kPilloryTag}, decoded));
  KCD2S_CHECK(!decode_pillory({kPilloryTag, kPilloryPutIn}, decoded));
  KCD2S_CHECK(!decode_pillory({kPilloryTag, kPilloryRelease, 'x'}, decoded));
  KCD2S_CHECK(!decode_pillory({std::uint8_t(kPilloryTag + 1), kPilloryRelease}, decoded));
}

namespace {

PilloryReport stay_report(std::uint8_t reason) {
  PilloryReport report;
  report.reason = reason;
  return report;
}

PilloryReport lock_report(const std::string& lock) {
  PilloryReport report;
  report.reason = kPilloryLockOpened;
  report.lock = lock;
  return report;
}

const char* const kLiveLock =
    "pillory_lockpickable[Pillory/pillory1:pillory_prefab[Pillory/pillory1]_"
    "2dca5774-55cc-0a7c-1b1c-1432c8cc3f7e]";

}  // namespace

KCD2S_TEST(pillory, reports_round_trip_through_the_codec) {
  PilloryReport decoded;
  for (std::uint8_t r : {kPilloryLockpicked, kPilloryLeft}) {
    KCD2S_CHECK(decode_pillory_report(encode_pillory_report(stay_report(r)), decoded));
    KCD2S_CHECK(decoded == stay_report(r));
  }
  // A lock-opened report carries the name of the lock (the live one is 98 characters).
  KCD2S_CHECK(decode_pillory_report(encode_pillory_report(lock_report(kLiveLock)), decoded));
  KCD2S_CHECK(decoded == lock_report(kLiveLock));
  KCD2S_CHECK(decode_pillory_report(
      encode_pillory_report(lock_report(std::string(kPilloryNameMax, 'a'))), decoded));
  KCD2S_CHECK_EQ(decoded.lock.size(), kPilloryNameMax);
  // The nearest pillory the client put the player into carries its holder's name.
  PilloryReport entered;
  entered.reason = kPilloryEntered;
  entered.lock = "so_pillory[Pillory/pillory1]";
  KCD2S_CHECK(decode_pillory_report(encode_pillory_report(entered), decoded));
  KCD2S_CHECK(decoded == entered);

  // The codec refuses what cannot travel: an unknown reason, a stay with a text, a lock-opened
  // or entered report without one or with a text that is too long or not printable.
  KCD2S_CHECK(encode_pillory_report(stay_report(0)).empty());
  KCD2S_CHECK(encode_pillory_report(stay_report(5)).empty());
  KCD2S_CHECK(encode_pillory_report(stay_report(kPilloryLockOpened)).empty());
  KCD2S_CHECK(encode_pillory_report(stay_report(kPilloryEntered)).empty());
  PilloryReport bad = stay_report(kPilloryLeft);
  bad.lock = "pillory_lockpickable";
  KCD2S_CHECK(encode_pillory_report(bad).empty());
  KCD2S_CHECK(encode_pillory_report(lock_report(std::string(kPilloryNameMax + 1, 'a'))).empty());
  KCD2S_CHECK(encode_pillory_report(lock_report("tab\there")).empty());

  // The decoder rejects a bad length or text.
  KCD2S_CHECK(!decode_pillory_report({kPilloryTag, 0}, decoded));
  KCD2S_CHECK(!decode_pillory_report({kPilloryTag, 5}, decoded));
  KCD2S_CHECK(!decode_pillory_report({kPilloryTag, kPilloryEntered}, decoded));
  KCD2S_CHECK(!decode_pillory_report({kPilloryTag}, decoded));
  KCD2S_CHECK(!decode_pillory_report({kPilloryTag, kPilloryLockpicked, 0}, decoded));
  KCD2S_CHECK(!decode_pillory_report({kPilloryTag, kPilloryLeft, 'x'}, decoded));
  KCD2S_CHECK(!decode_pillory_report({kPilloryTag, kPilloryLockOpened}, decoded));
  KCD2S_CHECK(!decode_pillory_report({kPilloryTag, kPilloryLockOpened, 'a', '\t'}, decoded));
  std::vector<std::uint8_t> too_long = {kPilloryTag, kPilloryLockOpened};
  too_long.insert(too_long.end(), kPilloryNameMax + 1, std::uint8_t('a'));
  KCD2S_CHECK(!decode_pillory_report(too_long, decoded));
  KCD2S_CHECK(!decode_pillory_report({std::uint8_t(kPilloryTag + 1), 1}, decoded));
}

KCD2S_TEST(pillory, orders_reach_the_player_and_reports_reach_the_server) {
  Link link;
  PilloryOrder put_in;
  put_in.kind = kPilloryPutIn;
  put_in.holder = "pillory_village";
  PilloryOrder release;
  release.kind = kPilloryRelease;

  // No world yet on the client: nothing travels back.
  KCD2S_CHECK(!link.client.send_pillory_report(stay_report(kPilloryLeft)));

  KCD2S_CHECK(link.client.mark_world_ready(1));
  link.run(5);
  KCD2S_CHECK(link.server.send_pillory(link.player, put_in));
  KCD2S_CHECK(link.server.send_pillory(link.player, release));
  // An unknown player, an order the codec refuses.
  KCD2S_CHECK(!link.server.send_pillory(link.player + 1, put_in));
  PilloryOrder empty = put_in;
  empty.holder.clear();
  KCD2S_CHECK(!link.server.send_pillory(link.player, empty));
  link.run(5);
  const auto orders = link.client.take_pillory_orders();
  KCD2S_CHECK_EQ(orders.size(), std::size_t(2));
  if (orders.size() == 2) {
    KCD2S_CHECK(orders[0] == put_in);
    KCD2S_CHECK(orders[1] == release);
  }
  KCD2S_CHECK(link.client.take_pillory_orders().empty());

  KCD2S_CHECK(link.client.send_pillory_report(stay_report(kPilloryLockpicked)));
  KCD2S_CHECK(link.client.send_pillory_report(stay_report(kPilloryLeft)));
  KCD2S_CHECK(link.client.send_pillory_report(lock_report(kLiveLock)));
  KCD2S_CHECK(!link.client.send_pillory_report(stay_report(0)));
  KCD2S_CHECK(!link.client.send_pillory_report(lock_report("")));
  link.run(5);
  const auto releases = link.server.take_pillory_releases();
  KCD2S_CHECK_EQ(releases.size(), std::size_t(3));
  if (releases.size() == 3) {
    KCD2S_CHECK_EQ(releases[0].first, link.player);
    KCD2S_CHECK(releases[0].second == stay_report(kPilloryLockpicked));
    KCD2S_CHECK(releases[1].second == stay_report(kPilloryLeft));
    KCD2S_CHECK(releases[2].second == lock_report(kLiveLock));
  }
  KCD2S_CHECK(link.server.take_pillory_releases().empty());
}

KCD2S_TEST(pillory, a_burst_is_bounded_on_both_sides) {
  Link link;
  KCD2S_CHECK(link.client.mark_world_ready(1));
  link.run(5);
  // A client keeps the newest orders.
  for (std::size_t i = 1; i <= kMaxPilloryOrders + 3; ++i) {
    PilloryOrder order;
    order.holder = "holder" + std::to_string(i);
    KCD2S_CHECK(link.server.send_pillory(link.player, order));
  }
  link.run(10);
  const auto orders = link.client.take_pillory_orders();
  KCD2S_CHECK_EQ(orders.size(), kMaxPilloryOrders);
  if (orders.size() == kMaxPilloryOrders) {
    KCD2S_CHECK(orders.front().holder == "holder4");
    KCD2S_CHECK(orders.back().holder == "holder" + std::to_string(kMaxPilloryOrders + 3));
  }
  // The server takes at most kMaxPilloryReportsPerSecond reports of a client per second.
  for (unsigned i = 0; i < kMaxPilloryReportsPerSecond + 5; ++i) {
    KCD2S_CHECK(link.client.send_pillory_report(stay_report(kPilloryLeft)));
  }
  link.run(10);
  KCD2S_CHECK_EQ(link.server.take_pillory_releases().size(),
                 std::size_t(kMaxPilloryReportsPerSecond));
}

KCD2S_TEST(pillory, a_new_world_drops_the_orders_of_the_old_one) {
  Link link;
  KCD2S_CHECK(link.client.mark_world_ready(1));
  link.run(5);
  PilloryOrder put_in;
  put_in.holder = "pillory_village";
  KCD2S_CHECK(link.server.send_pillory(link.player, put_in));
  link.run(5);
  // The order arrived but the game did not take it before the world changed.
  WorldConfig next;
  next.revision = 2;
  KCD2S_CHECK(link.server.configure_world(link.peer, next));
  link.run(10);
  KCD2S_CHECK(link.client.take_pillory_orders().empty());
}
