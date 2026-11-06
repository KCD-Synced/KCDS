#include "kcd2s/integration/server.hpp"
#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/vendor.hpp"
#include "kcd2s/testkit.hpp"
using namespace kcd2s;
using namespace kcd2s::integration;

// VENDOR_SHOW, VENDOR_CLOSE, VENDOR_TRADE and VENDOR_CLOSED: a vendor's shelf
// on one player's shop screen and the basket the player confirms there.
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

VendorShow show_of(std::uint32_t session) {
  VendorShow show;
  show.session = session;
  show.open = true;
  show.buys = true;
  show.purse_limited = true;
  show.npc = 4100;
  show.revision = 12;
  show.purse = 2500;
  show.money[0] = 0x5A;
  VendorShelfRow apples;
  apples.row.id = "s1";
  apples.row.item[3] = 7;
  apples.row.amount = 10;
  apples.price = 15;
  VendorShelfRow sword;
  sword.row.id = "s2";
  sword.row.item[0] = 1;
  sword.row.amount = 1;
  sword.row.quality = 3;
  sword.row.health = 0.75f;
  sword.price = kMaxVendorPrice;
  show.shelf = {apples, sword};
  VendorBuyPrice buy;
  buy.item[15] = 9;
  buy.price = 5;
  show.buy_prices = {buy};
  return show;
}

VendorTrade trade_of(std::uint32_t session) {
  VendorTrade trade;
  trade.session = session;
  trade.revision = 12;
  trade.bought = {{"s1", 3}, {"s2", 1}};
  trade.sold = {{"i14", 2}};
  return trade;
}

}  // namespace

KCD2S_TEST(vendor, a_shelf_round_trips_through_the_codec) {
  const VendorShow show = show_of(7);
  VendorShow heard;
  const auto bytes = encode_vendor_show(show);
  KCD2S_CHECK(!bytes.empty() && bytes[0] == kVendorShowTag);
  KCD2S_CHECK(decode_vendor_show(bytes, heard) && heard == show);
  // An empty shelf with a purse that never runs out, from a vendor that does not buy.
  VendorShow bare = show_of(0xFFFFFFFFu);
  bare.open = false;
  bare.buys = false;
  bare.purse_limited = false;
  bare.purse = 0;
  bare.npc = 0;
  bare.shelf.clear();
  bare.buy_prices.clear();
  KCD2S_CHECK(decode_vendor_show(encode_vendor_show(bare), heard) && heard == bare);
  // A player's trade: the keeper is the seller, a player.
  VendorShow trade = show_of(9);
  trade.npc = 3;
  trade.keeper_player = true;
  KCD2S_CHECK(decode_vendor_show(encode_vendor_show(trade), heard) && heard == trade &&
              heard.keeper_player);
  // What cannot travel: no session, too many rows, a row without an id, a price too high.
  VendorShow bad = show_of(0);
  KCD2S_CHECK(encode_vendor_show(bad).empty());
  bad = show_of(1);
  // A copy of the row: assign must not be given a reference into the vector it refills.
  const VendorShelfRow row = bad.shelf[0];
  bad.shelf.assign(kMaxVendorRows + 1, row);
  KCD2S_CHECK(encode_vendor_show(bad).empty());
  bad = show_of(1);
  bad.shelf[0].row.id.clear();
  KCD2S_CHECK(encode_vendor_show(bad).empty());
  bad = show_of(1);
  bad.shelf[0].price = kMaxVendorPrice + 1;
  KCD2S_CHECK(encode_vendor_show(bad).empty());
  bad = show_of(1);
  bad.purse = kMaxVendorPrice + 1;
  KCD2S_CHECK(encode_vendor_show(bad).empty());
  // A cut or padded message, another tag.
  auto cut = bytes;
  cut.pop_back();
  KCD2S_CHECK(!decode_vendor_show(cut, heard));
  auto padded = bytes;
  padded.push_back(0);
  KCD2S_CHECK(!decode_vendor_show(padded, heard));
  auto other = bytes;
  other[0] = kVendorCloseTag;
  KCD2S_CHECK(!decode_vendor_show(other, heard));
}

KCD2S_TEST(vendor, baskets_and_closes_round_trip_through_the_codec) {
  const VendorTrade trade = trade_of(42);
  VendorTrade heard;
  const auto bytes = encode_vendor_trade(trade);
  KCD2S_CHECK(decode_vendor_trade(bytes, heard) && heard == trade);
  VendorTrade sell_only = trade_of(42);
  sell_only.bought.clear();
  KCD2S_CHECK(decode_vendor_trade(encode_vendor_trade(sell_only), heard) && heard == sell_only);
  // An empty basket, a line without units or a row: nothing travels.
  VendorTrade bad = trade_of(42);
  bad.bought.clear();
  bad.sold.clear();
  KCD2S_CHECK(encode_vendor_trade(bad).empty());
  bad = trade_of(42);
  bad.bought[0].units = 0;
  KCD2S_CHECK(encode_vendor_trade(bad).empty());
  bad = trade_of(42);
  bad.sold[0].row.clear();
  KCD2S_CHECK(encode_vendor_trade(bad).empty());
  KCD2S_CHECK(encode_vendor_trade(trade_of(0)).empty());
  auto cut = bytes;
  cut.pop_back();
  KCD2S_CHECK(!decode_vendor_trade(cut, heard));

  VendorClose close;
  close.session = 42;
  VendorClose heard_close;
  KCD2S_CHECK(decode_vendor_close(encode_vendor_close(close), heard_close) && heard_close == close);
  KCD2S_CHECK(encode_vendor_close(VendorClose{}).empty());

  VendorClosed closed;
  closed.session = 42;
  closed.reason = VendorClosedReason::Failed;
  VendorClosed heard_closed;
  const auto closed_bytes = encode_vendor_closed(closed);
  KCD2S_CHECK(decode_vendor_closed(closed_bytes, heard_closed) && heard_closed == closed);
  // Only the player's side reasons travel: the server closed it (1) and replaced (2) are its own.
  auto server_reason = closed_bytes;
  server_reason.back() = 1;
  KCD2S_CHECK(!decode_vendor_closed(server_reason, heard_closed));
  closed.reason = VendorClosedReason(2);
  KCD2S_CHECK(encode_vendor_closed(closed).empty());
}

KCD2S_TEST(vendor, shelves_and_closes_reach_the_player_in_order) {
  Link link;
  // No ready world yet: the shelf reaches nobody.
  KCD2S_CHECK(!link.server.send_vendor_show(link.player, show_of(1)));
  KCD2S_CHECK(link.client.mark_world_ready(1));
  link.run(5);
  KCD2S_CHECK(link.server.send_vendor_show(link.player, show_of(1)));
  VendorShow refresh = show_of(1);
  refresh.open = false;
  refresh.revision = 13;
  KCD2S_CHECK(link.server.send_vendor_show(link.player, refresh));
  VendorClose close;
  close.session = 1;
  KCD2S_CHECK(link.server.send_vendor_close(link.player, close));
  KCD2S_CHECK(!link.server.send_vendor_show(link.player + 1, show_of(2)));
  link.run(5);
  const auto orders = link.client.take_vendor_orders();
  KCD2S_CHECK_EQ(orders.size(), std::size_t(3));
  if (orders.size() == 3) {
    KCD2S_CHECK(std::holds_alternative<VendorShow>(orders[0]) &&
                std::get<VendorShow>(orders[0]) == show_of(1));
    KCD2S_CHECK(std::holds_alternative<VendorShow>(orders[1]) &&
                std::get<VendorShow>(orders[1]) == refresh);
    KCD2S_CHECK(std::holds_alternative<VendorClose>(orders[2]) &&
                std::get<VendorClose>(orders[2]) == close);
  }
  KCD2S_CHECK(link.client.take_vendor_orders().empty());
}

KCD2S_TEST(vendor, baskets_and_closes_reach_the_server_bounded) {
  Link link;
  const VendorTrade trade = trade_of(5);
  KCD2S_CHECK(!link.client.send_vendor_trade(trade));
  KCD2S_CHECK(link.client.mark_world_ready(1));
  link.run(5);
  KCD2S_CHECK(link.client.send_vendor_trade(trade));
  VendorClosed closed;
  closed.session = 5;
  closed.reason = VendorClosedReason::Player;
  KCD2S_CHECK(link.client.send_vendor_closed(closed));
  link.run(5);
  const auto trades = link.server.take_vendor_trades();
  KCD2S_CHECK_EQ(trades.size(), std::size_t(1));
  if (trades.size() == 1) {
    KCD2S_CHECK_EQ(trades[0].first, link.player);
    KCD2S_CHECK(trades[0].second == trade);
  }
  const auto closes = link.server.take_vendor_closes();
  KCD2S_CHECK_EQ(closes.size(), std::size_t(1));
  if (closes.size() == 1) {
    KCD2S_CHECK(closes[0].second == closed);
  }
  link.run(70);
  for (unsigned i = 0; i < kMaxVendorReportsPerSecond + 4; ++i) {
    KCD2S_CHECK(link.client.send_vendor_trade(trade));
  }
  link.run(10);
  KCD2S_CHECK_EQ(link.server.take_vendor_trades().size(), std::size_t(kMaxVendorReportsPerSecond));
}

// VENDOR_HAGGLE and VENDOR_HAGGLE_REPLY (0.4.1): an offer in the game's haggle and the trader's
// answer.
KCD2S_TEST(vendor, haggle_offers_and_answers_round_trip_through_the_codec) {
  VendorShow show = show_of(7);
  show.haggles = true;
  VendorShow heard_show;
  KCD2S_CHECK(decode_vendor_show(encode_vendor_show(show), heard_show) && heard_show.haggles);

  VendorHaggle haggle;
  haggle.session = 7;
  haggle.revision = 12;
  haggle.round = 3;
  haggle.offered = -850;
  haggle.bought = {{"s1", 2}};
  haggle.sold = {{"i14", 1}};
  VendorHaggle heard;
  const auto bytes = encode_vendor_haggle(haggle);
  KCD2S_CHECK(!bytes.empty() && bytes[0] == kVendorHaggleTag);
  KCD2S_CHECK(decode_vendor_haggle(bytes, heard) && heard == haggle);
  haggle.offered = kMaxVendorPrice;
  KCD2S_CHECK(decode_vendor_haggle(encode_vendor_haggle(haggle), heard) && heard == haggle);
  // What cannot travel: no round, a price too high, nothing in the basket.
  VendorHaggle bad = haggle;
  bad.round = 0;
  KCD2S_CHECK(encode_vendor_haggle(bad).empty());
  bad = haggle;
  bad.round = kMaxVendorHaggleRounds + 1;
  KCD2S_CHECK(encode_vendor_haggle(bad).empty());
  bad = haggle;
  bad.offered = -std::int64_t(kMaxVendorPrice) - 1;
  KCD2S_CHECK(encode_vendor_haggle(bad).empty());
  bad = haggle;
  bad.bought.clear();
  bad.sold.clear();
  KCD2S_CHECK(encode_vendor_haggle(bad).empty());
  auto cut = bytes;
  cut.pop_back();
  KCD2S_CHECK(!decode_vendor_haggle(cut, heard));

  VendorHaggleReply reply;
  reply.session = 7;
  reply.round = 3;
  reply.answer = VendorHaggleAnswer::Counter;
  reply.price = 900;
  reply.mood = 68;
  VendorHaggleReply heard_reply;
  const auto reply_bytes = encode_vendor_haggle_reply(reply);
  KCD2S_CHECK(decode_vendor_haggle_reply(reply_bytes, heard_reply) && heard_reply == reply);
  reply.answer = VendorHaggleAnswer::Accept;
  reply.price = -40;
  KCD2S_CHECK(decode_vendor_haggle_reply(encode_vendor_haggle_reply(reply), heard_reply) &&
              heard_reply == reply);
  VendorHaggleReply bad_reply = reply;
  bad_reply.mood = kMaxVendorHaggleMood + 1;
  KCD2S_CHECK(encode_vendor_haggle_reply(bad_reply).empty());
  bad_reply = reply;
  bad_reply.answer = VendorHaggleAnswer(5);
  KCD2S_CHECK(encode_vendor_haggle_reply(bad_reply).empty());
  auto other = reply_bytes;
  other[0] = kVendorHaggleTag;
  KCD2S_CHECK(!decode_vendor_haggle_reply(other, heard_reply));
}

KCD2S_TEST(vendor, haggle_offers_reach_the_server_and_answers_the_player) {
  Link link;
  VendorHaggle haggle;
  haggle.session = 5;
  haggle.revision = 12;
  haggle.round = 1;
  haggle.offered = 900;
  haggle.bought = {{"s1", 1}};
  KCD2S_CHECK(!link.client.send_vendor_haggle(haggle));
  KCD2S_CHECK(link.client.mark_world_ready(1));
  link.run(5);
  KCD2S_CHECK(link.client.send_vendor_haggle(haggle));
  link.run(5);
  const auto haggles = link.server.take_vendor_haggles();
  KCD2S_CHECK_EQ(haggles.size(), std::size_t(1));
  if (haggles.size() == 1) {
    KCD2S_CHECK_EQ(haggles[0].first, link.player);
    KCD2S_CHECK(haggles[0].second == haggle);
  }
  VendorHaggleReply reply;
  reply.session = 5;
  reply.round = 1;
  reply.answer = VendorHaggleAnswer::Accept;
  reply.price = 900;
  KCD2S_CHECK(link.server.send_vendor_haggle_reply(link.player, reply));
  KCD2S_CHECK(!link.server.send_vendor_haggle_reply(link.player + 1, reply));
  link.run(5);
  const auto orders = link.client.take_vendor_orders();
  KCD2S_CHECK_EQ(orders.size(), std::size_t(1));
  if (orders.size() == 1) {
    KCD2S_CHECK(std::holds_alternative<VendorHaggleReply>(orders[0]) &&
                std::get<VendorHaggleReply>(orders[0]) == reply);
  }
}
