#include "kcd2s/transport/datagram.hpp"

#include <cstdint>

#include "kcd2s/testkit.hpp"
#include "kcd2s/wire/packet.hpp"

using kcd2s::transport::Datagram;
using kcd2s::transport::LinkParams;
using kcd2s::transport::make_loopback_pair;
using kcd2s::transport::make_sim_pair;

static Datagram bytes(std::initializer_list<std::uint8_t> b) {
  return Datagram(b);
}

KCD2S_TEST(datagram, loopback_delivers_in_order) {
  auto [a, b] = make_loopback_pair();
  a->send(bytes({1, 2, 3}));
  a->send(bytes({4, 5}));

  Datagram out;
  KCD2S_CHECK(b->recv(out));
  KCD2S_CHECK_EQ(out.size(), std::size_t{3});
  KCD2S_CHECK_EQ(out[0], std::uint8_t{1});
  KCD2S_CHECK(b->recv(out));
  KCD2S_CHECK_EQ(out[0], std::uint8_t{4});
  KCD2S_CHECK(!b->recv(out));  // nothing left
}

KCD2S_TEST(datagram, bidirectional) {
  auto [a, b] = make_loopback_pair();
  a->send(bytes({0xAA}));
  b->send(bytes({0xBB}));

  Datagram out;
  KCD2S_CHECK(b->recv(out));
  KCD2S_CHECK_EQ(out[0], std::uint8_t{0xAA});
  KCD2S_CHECK(a->recv(out));
  KCD2S_CHECK_EQ(out[0], std::uint8_t{0xBB});
}

KCD2S_TEST(datagram, carries_a_real_kstp_packet) {
  auto [a, b] = make_loopback_pair();
  kcd2s::wire::Packet p;
  p.peerSeq = 42;
  kcd2s::wire::ChannelPayload cp;
  cp.channel = static_cast<std::uint8_t>(kcd2s::wire::Channel::Snapshot);
  cp.bytes = {9, 8, 7};
  p.channels.push_back(cp);

  a->send(kcd2s::wire::write_packet(p));

  Datagram raw;
  KCD2S_CHECK(b->recv(raw));
  kcd2s::wire::Packet got;
  KCD2S_CHECK(kcd2s::wire::read_packet(raw, got));
  KCD2S_CHECK_EQ(got.peerSeq, std::uint16_t{42});
  KCD2S_CHECK_EQ(got.channels.size(), std::size_t{1});
  KCD2S_CHECK_EQ(got.channels[0].bytes[0], std::uint8_t{9});
}

KCD2S_TEST(datagram, delay_requires_polls) {
  LinkParams p;
  p.min_delay = 3;
  p.max_delay = 3;
  auto [a, b] = make_sim_pair(p, LinkParams{});
  a->send(bytes({1}));

  Datagram out;
  KCD2S_CHECK(!b->recv(out));  // not yet
  b->poll();
  KCD2S_CHECK(!b->recv(out));  // step 1
  b->poll();
  KCD2S_CHECK(!b->recv(out));  // step 2
  b->poll();
  KCD2S_CHECK(b->recv(out));  // step 3 -> matured
  KCD2S_CHECK_EQ(out[0], std::uint8_t{1});
}

KCD2S_TEST(datagram, total_loss_drops_everything) {
  LinkParams p;
  p.loss = 1.0;  // drop all
  p.seed = 123;
  auto [a, b] = make_sim_pair(p, LinkParams{});
  for (int i = 0; i < 20; ++i)
    a->send(bytes({static_cast<std::uint8_t>(i)}));
  for (int i = 0; i < 5; ++i)
    b->poll();

  Datagram out;
  KCD2S_CHECK(!b->recv(out));
}

KCD2S_TEST(datagram, partial_loss_is_seed_deterministic) {
  auto run = []() {
    LinkParams p;
    p.loss = 0.5;
    p.seed = 0xC0FFEE;
    auto pair = make_sim_pair(p, LinkParams{});
    auto& a = pair.first;
    auto& b = pair.second;
    int delivered = 0;
    for (int i = 0; i < 200; ++i)
      a->send(bytes({static_cast<std::uint8_t>(i)}));
    Datagram out;
    while (b->recv(out))
      ++delivered;
    return delivered;
  };
  const int first = run();
  const int second = run();
  KCD2S_CHECK_EQ(first, second);          // same seed => same drops
  KCD2S_CHECK(first > 0 && first < 200);  // roughly half, certainly not all/none
}

KCD2S_TEST(datagram, duplicate_delivers_twice) {
  LinkParams p;
  p.duplicate = 1.0;  // always duplicate
  p.seed = 1;
  auto [a, b] = make_sim_pair(p, LinkParams{});
  a->send(bytes({0x55}));

  Datagram out;
  KCD2S_CHECK(b->recv(out));
  KCD2S_CHECK_EQ(out[0], std::uint8_t{0x55});
  KCD2S_CHECK(b->recv(out));  // the duplicate
  KCD2S_CHECK_EQ(out[0], std::uint8_t{0x55});
  KCD2S_CHECK(!b->recv(out));
}

KCD2S_TEST(datagram, variable_delay_can_reorder) {
  // With a spread of delays, later-sent datagrams can mature before earlier ones.
  LinkParams p;
  p.min_delay = 1;
  p.max_delay = 10;
  p.seed = 777;
  auto [a, b] = make_sim_pair(p, LinkParams{});
  const int n = 50;
  for (int i = 0; i < n; ++i)
    a->send(bytes({static_cast<std::uint8_t>(i)}));

  std::vector<std::uint8_t> order;
  for (int step = 0; step < 20; ++step) {
    b->poll();
    Datagram out;
    while (b->recv(out))
      order.push_back(out[0]);
  }
  KCD2S_CHECK_EQ(order.size(), static_cast<std::size_t>(n));  // all eventually delivered
  bool reordered = false;
  for (std::size_t i = 1; i < order.size(); ++i)
    if (order[i] < order[i - 1])
      reordered = true;
  KCD2S_CHECK(reordered);  // the spread produced at least one out-of-order delivery
}
