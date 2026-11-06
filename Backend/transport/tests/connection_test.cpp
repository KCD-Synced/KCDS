#include "kcd2s/transport/connection.hpp"

#include <cstdint>
#include <vector>

#include "kcd2s/testkit.hpp"
#include "kcd2s/transport/datagram.hpp"
#include "kcd2s/wire/packet.hpp"

using kcd2s::transport::Connection;
using kcd2s::transport::DeliveryTier;
using kcd2s::transport::LinkParams;
using kcd2s::transport::make_loopback_pair;
using kcd2s::transport::make_sim_pair;
using kcd2s::wire::Channel;

static std::vector<std::uint8_t> one(std::uint8_t v) {
  return {v};
}

KCD2S_TEST(connection, unreliable_delivers_once) {
  auto [a, b] = make_loopback_pair();
  Connection ca(a.get()), cb(b.get());
  ca.queue(static_cast<std::uint8_t>(Channel::Snapshot), one(7));
  ca.flush(0.0);

  auto got = cb.receive(0.0);
  KCD2S_CHECK_EQ(got.size(), std::size_t{1});
  KCD2S_CHECK_EQ(got[0].channel, static_cast<std::uint8_t>(Channel::Snapshot));
  KCD2S_CHECK_EQ(got[0].bytes[0], std::uint8_t{7});
}

KCD2S_TEST(connection, coalesces_multiple_channels_one_datagram) {
  auto [a, b] = make_loopback_pair();
  Connection ca(a.get()), cb(b.get());
  ca.queue(static_cast<std::uint8_t>(Channel::Snapshot), one(1));
  ca.queue(static_cast<std::uint8_t>(Channel::Upstream), one(2));
  ca.flush(0.0);

  auto got = cb.receive(0.0);
  KCD2S_CHECK_EQ(got.size(), std::size_t{2});  // both rode one datagram
}

KCD2S_TEST(connection, reliable_ack_clears_outstanding) {
  auto [a, b] = make_loopback_pair();
  Connection ca(a.get()), cb(b.get());
  ca.queue(static_cast<std::uint8_t>(Channel::Events), one(42));  // reliable-ordered
  ca.flush(0.0);
  KCD2S_CHECK_EQ(ca.outstanding_reliable(), std::size_t{1});

  (void)cb.receive(0.0);  // B gets the message
  cb.flush(0.0);          // B sends an ack back
  (void)ca.receive(0.0);  // A processes the ack
  KCD2S_CHECK_EQ(ca.outstanding_reliable(), std::size_t{0});
}

KCD2S_TEST(connection, reliable_ordered_survives_loss_in_order) {
  LinkParams a2b;
  a2b.loss = 0.5;
  a2b.seed = 0x1234;
  auto [a, b] = make_sim_pair(a2b, LinkParams{});  // acks (b->a) lossless
  Connection ca(a.get()), cb(b.get());

  const int n = 15;
  for (int i = 0; i < n; ++i)
    ca.queue(static_cast<std::uint8_t>(Channel::Events), one(i));

  std::vector<std::uint8_t> got;
  double t = 0.0;
  for (int cycle = 0; cycle < 80; ++cycle) {
    ca.flush(t);  // resends all unacked each flush
    for (int k = 0; k < 20; ++k)
      b->poll();
    for (auto& m : cb.receive(t))
      got.push_back(m.bytes[0]);
    cb.flush(t);  // ack
    for (int k = 0; k < 20; ++k)
      a->poll();
    (void)ca.receive(t);
    t += 0.01;
  }

  KCD2S_CHECK_EQ(got.size(), static_cast<std::size_t>(n));  // all delivered, no dups
  bool ordered = got.size() == static_cast<std::size_t>(n);
  for (std::size_t i = 0; i < got.size(); ++i)
    if (got[i] != static_cast<std::uint8_t>(i))
      ordered = false;
  KCD2S_CHECK(ordered);
  KCD2S_CHECK_EQ(ca.outstanding_reliable(), std::size_t{0});
}

KCD2S_TEST(connection, reliable_unordered_dedups_duplicates) {
  LinkParams a2b;
  a2b.duplicate = 1.0;  // always duplicate
  a2b.seed = 9;
  auto [a, b] = make_sim_pair(a2b, LinkParams{});
  const std::uint8_t ch = 10;
  Connection ca(a.get()), cb(b.get());
  ca.set_channel_tier(ch, DeliveryTier::ReliableUnordered);
  cb.set_channel_tier(ch, DeliveryTier::ReliableUnordered);

  const int n = 8;
  for (int i = 0; i < n; ++i)
    ca.queue(ch, one(i));

  std::vector<int> counts(n, 0);
  double t = 0.0;
  for (int cycle = 0; cycle < 40; ++cycle) {
    ca.flush(t);
    for (auto& m : cb.receive(t))
      counts[m.bytes[0]]++;
    cb.flush(t);
    (void)ca.receive(t);
    t += 0.01;
  }
  for (int i = 0; i < n; ++i)
    KCD2S_CHECK_EQ(counts[i], 1);  // each delivered exactly once
}

KCD2S_TEST(connection, sequenced_keeps_newest_only) {
  LinkParams a2b;  // spread of delays -> reordering; no loss so every packet arrives
  a2b.min_delay = 1;
  a2b.max_delay = 15;
  a2b.seed = 555;
  auto [a, b] = make_sim_pair(a2b, LinkParams{});
  Connection ca(a.get()), cb(b.get());  // Voice defaults to UnreliableSequenced

  const int n = 60;
  double t = 0.0;
  std::vector<std::uint8_t> got;
  // Each message in its own datagram so delays reorder them.
  for (int i = 0; i < n; ++i) {
    ca.queue(static_cast<std::uint8_t>(Channel::Voice), one(static_cast<std::uint8_t>(i)));
    ca.flush(t);
    t += 0.01;
  }
  for (int step = 0; step < 40; ++step) {
    b->poll();
    for (auto& m : cb.receive(t))
      got.push_back(m.bytes[0]);
  }

  bool strictly_increasing = true;
  for (std::size_t i = 1; i < got.size(); ++i)
    if (got[i] <= got[i - 1])
      strictly_increasing = false;
  KCD2S_CHECK(strictly_increasing);  // never an older-after-newer
  KCD2S_CHECK(!got.empty() && got.size() < static_cast<std::size_t>(n));  // reorder dropped some
  KCD2S_CHECK_EQ(got.back(), static_cast<std::uint8_t>(n - 1));           // newest always delivered
}

KCD2S_TEST(connection, reliable_not_resent_every_flush) {
  // A fresh unacked reliable message must wait one RTO between (re)transmits, not ride every flush.
  auto [a, b] = make_loopback_pair();
  Connection ca(a.get()), cb(b.get());
  ca.set_retransmit_params(2.0, 0.05, 1.0);  // RTO floor 50ms (rtt still 0)
  (void)cb;

  ca.queue(static_cast<std::uint8_t>(Channel::Events), one(1));
  ca.flush(0.0);
  KCD2S_CHECK_EQ(ca.reliable_sent_count(), std::uint64_t{1});  // first send

  // b never acks. Flushing within the RTO window does NOT resend.
  ca.flush(0.01);
  ca.flush(0.02);
  ca.flush(0.04);
  KCD2S_CHECK_EQ(ca.reliable_sent_count(), std::uint64_t{1});

  ca.flush(0.06);  // past one RTO from the 0.0 send -> exactly one retransmit
  KCD2S_CHECK_EQ(ca.reliable_sent_count(), std::uint64_t{2});
  ca.flush(0.07);  // within the next RTO window -> no extra
  KCD2S_CHECK_EQ(ca.reliable_sent_count(), std::uint64_t{2});
  ca.flush(0.12);  // another RTO elapsed -> one more
  KCD2S_CHECK_EQ(ca.reliable_sent_count(), std::uint64_t{3});
  KCD2S_CHECK_EQ(ca.reliable_retransmit_count(), std::uint64_t{2});
}

KCD2S_TEST(connection, reliable_retransmits_recover_under_loss) {
  LinkParams a2b;
  a2b.loss = 0.4;
  a2b.seed = 0x7;
  auto [a, b] = make_sim_pair(a2b, LinkParams{});  // acks (b->a) lossless
  Connection ca(a.get()), cb(b.get());
  ca.set_retransmit_params(2.0, 0.03, 1.0);

  ca.queue(static_cast<std::uint8_t>(Channel::Events), one(99));

  std::vector<std::uint8_t> got;
  double t = 0.0;
  for (int cycle = 0; cycle < 60; ++cycle) {
    ca.flush(t);
    for (int k = 0; k < 5; ++k)
      b->poll();
    for (auto& m : cb.receive(t))
      got.push_back(m.bytes[0]);
    cb.flush(t);
    for (int k = 0; k < 5; ++k)
      a->poll();
    (void)ca.receive(t);
    t += 0.02;
  }
  KCD2S_CHECK_EQ(got.size(), std::size_t{1});  // delivered exactly once despite 40% loss
  KCD2S_CHECK_EQ(got[0], std::uint8_t{99});
  KCD2S_CHECK_EQ(ca.outstanding_reliable(), std::size_t{0});
  // RTO-gated: far fewer sends than resend-every-flush (60) would have produced.
  KCD2S_CHECK(ca.reliable_sent_count() >= std::uint64_t{1});
  KCD2S_CHECK(ca.reliable_sent_count() < std::uint64_t{60});
}

KCD2S_TEST(connection, rto_adapts_to_measured_rtt) {
  auto [a, b] = make_loopback_pair();
  Connection ca(a.get()), cb(b.get());
  ca.set_retransmit_params(2.0, 0.05, 1.0);
  KCD2S_CHECK_EQ(ca.retransmit_timeout(), 0.05);  // no RTT yet -> floor

  // One send/ack round with a 0.5s gap teaches the AckSystem a 0.5s RTT. Only a packet with a
  // payload measures it: a header-only packet's peer may stay quiet.
  ca.queue(static_cast<std::uint8_t>(Channel::Events), one(1));
  ca.flush(0.0);          // packet sent at t=0
  (void)cb.receive(0.0);  // b sees it
  cb.flush(0.0);          // b acks
  (void)ca.receive(0.5);  // a processes the ack at t=0.5 -> rtt sample 0.5s

  KCD2S_CHECK(ca.rtt_seconds() > 0.4);
  KCD2S_CHECK_EQ(ca.retransmit_timeout(), 1.0);  // clamp(0.5*2, .05, 1.0) == 1.0 (adapted up)
}

KCD2S_TEST(connection, liveness_tracks_last_receive) {
  auto [a, b] = make_loopback_pair();
  Connection ca(a.get()), cb(b.get());

  // No datagram received yet -> no liveness basis, never timed out.
  KCD2S_CHECK(!cb.has_received_datagram());
  KCD2S_CHECK(!cb.is_timed_out(100.0, 10.0));

  ca.flush(1.0);          // A sends (a keep-alive datagram)
  (void)cb.receive(1.0);  // B receives it at t=1
  KCD2S_CHECK(cb.has_received_datagram());
  KCD2S_CHECK_EQ(cb.last_recv_time(), 1.0);
  KCD2S_CHECK(!cb.is_timed_out(5.0, 10.0));  // 4s < 10s
  KCD2S_CHECK(cb.is_timed_out(20.0, 10.0));  // 19s > 10s -> timed out

  // A keeps talking -> liveness refreshes, not timed out.
  ca.flush(15.0);
  (void)cb.receive(15.0);
  KCD2S_CHECK_EQ(cb.last_recv_time(), 15.0);
  KCD2S_CHECK(!cb.is_timed_out(20.0, 10.0));  // only 5s since last recv
}

KCD2S_TEST(connection, protocol_gate_drops_mismatch) {
  auto [a, b] = make_loopback_pair();
  Connection cb(b.get());

  kcd2s::wire::Packet bad;
  bad.protocolId = 0x0000;  // wrong magic/version
  bad.channels.push_back({static_cast<std::uint8_t>(Channel::Snapshot), {1, 2, 3}});
  auto bytes = kcd2s::wire::write_packet(bad);
  a->send(bytes.data(), bytes.size());  // inject straight onto the wire

  auto got = cb.receive(0.0);
  KCD2S_CHECK_EQ(got.size(), std::size_t{0});
}

KCD2S_TEST(connection, a_burst_larger_than_a_packet_arrives_whole_and_in_order) {
  // 1000 reliable messages of 100 bytes queued at once (100 KB, more than a packet's 16-bit
  // channel length): they travel over several packets, all of them, in order.
  auto [a, b] = make_loopback_pair();
  Connection ca(a.get()), cb(b.get());
  constexpr int kMessages = 1000;
  for (int i = 0; i < kMessages; ++i) {
    std::vector<std::uint8_t> message(100, std::uint8_t(i & 0xff));
    message[0] = std::uint8_t(i >> 8);
    ca.queue(static_cast<std::uint8_t>(Channel::Events), message);
  }
  std::vector<int> got;
  for (int round = 0; round < 40 && int(got.size()) < kMessages; ++round) {
    ca.flush(double(round) * 0.01);
    for (const auto& m : cb.receive(double(round) * 0.01)) {
      got.push_back(int(m.bytes[0]) << 8 | int(m.bytes[1]));
    }
    cb.flush(double(round) * 0.01);
    (void)ca.receive(double(round) * 0.01);
  }
  KCD2S_CHECK_EQ(got.size(), std::size_t(kMessages));
  bool in_order = true;
  for (int i = 0; i < int(got.size()); ++i) {
    if (got[i] != i) {
      in_order = false;
    }
  }
  KCD2S_CHECK(in_order);
}
