#include "kcd2s/transport/endpoint.hpp"

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include "kcd2s/testkit.hpp"
#include "kcd2s/transport/connection.hpp"
#include "kcd2s/transport/datagram.hpp"
#include "kcd2s/transport/handshake.hpp"
#include "kcd2s/wire/packet.hpp"

using kcd2s::transport::ClientHandshake;
using kcd2s::transport::Connection;
using kcd2s::transport::Datagram;
using kcd2s::transport::IDatagramLink;
using kcd2s::transport::ServerEndpoint;
using kcd2s::wire::Channel;

namespace {

// Captures a client Connection's outbound datagrams (what it would sendto the server).
class CaptureLink : public IDatagramLink {
 public:
  using IDatagramLink::send;
  void send(const std::uint8_t* d, std::size_t n) override {
    out.emplace_back(d, d + n);
  }
  bool recv(Datagram&) override {
    return false;
  }
  void poll() override {
  }
  std::deque<Datagram> out;
};

// Server->client datagrams captured per address.
using Mailboxes = std::unordered_map<std::uint64_t, std::deque<Datagram>>;

// Run the full three-way handshake for `addr`; returns true if the client ends up established and
// the server admitted. `mail` receives the server's outbound datagrams via the endpoint's SendFn.
bool handshake(ServerEndpoint& sv, Mailboxes& mail, std::uint64_t addr, ClientHandshake& ch) {
  sv.feed(addr, ch.start(), 0.0);  // CONNECT_REQUEST -> server replies CHALLENGE
  if (mail[addr].empty())
    return false;
  Datagram challenge = mail[addr].front();
  mail[addr].pop_front();

  std::vector<std::uint8_t> response;
  if (!ch.on_datagram(challenge, response))
    return false;
  sv.feed(addr, response, 0.0);  // CHALLENGE_RESPONSE -> server admits + replies CONNECT_ACCEPT
  if (mail[addr].empty())
    return false;
  Datagram accept = mail[addr].front();
  mail[addr].pop_front();

  std::vector<std::uint8_t> none;
  ch.on_datagram(accept, none);
  return ch.established() && sv.admitted(addr);
}

}  // namespace

KCD2S_TEST(endpoint, handshake_then_channel_packet_delivered) {
  ServerEndpoint sv(0xABCDEF01u);
  Mailboxes mail;
  sv.set_send([&](std::uint64_t a, const Datagram& dg) { mail[a].push_back(dg); });

  int admits = 0;
  sv.set_on_admit([&](std::uint64_t, Connection&) { ++admits; });

  const std::uint64_t addr = 0xA1u;
  ClientHandshake ch(0xC0FFEEu);
  KCD2S_CHECK(handshake(sv, mail, addr, ch));
  KCD2S_CHECK_EQ(admits, 1);
  KCD2S_CHECK(sv.connection(addr) != nullptr);

  // Now the client sends a real KSTP channel packet via its own Connection.
  CaptureLink cl;
  Connection cc(&cl);
  cc.queue(static_cast<std::uint8_t>(Channel::Events), Datagram{1, 2, 3});
  cc.flush(0.0);
  KCD2S_CHECK(!cl.out.empty());

  auto delivered = sv.feed(addr, cl.out.front(), 0.0);
  KCD2S_CHECK_EQ(delivered.size(), std::size_t{1});
  KCD2S_CHECK_EQ(delivered[0].channel, static_cast<std::uint8_t>(Channel::Events));
  KCD2S_CHECK(delivered[0].bytes == (Datagram{1, 2, 3}));
}

KCD2S_TEST(endpoint, channel_traffic_before_handshake_ignored) {
  ServerEndpoint sv(0x1u);
  Mailboxes mail;
  sv.set_send([&](std::uint64_t a, const Datagram& dg) { mail[a].push_back(dg); });

  // A real channel packet from a source that has not completed the handshake.
  CaptureLink cl;
  Connection cc(&cl);
  cc.queue(static_cast<std::uint8_t>(Channel::Snapshot), Datagram{9});
  cc.flush(0.0);

  auto delivered = sv.feed(0xB2u, cl.out.front(), 0.0);
  KCD2S_CHECK(delivered.empty());
  KCD2S_CHECK(!sv.admitted(0xB2u));
  KCD2S_CHECK(sv.connection(0xB2u) == nullptr);
}

KCD2S_TEST(endpoint, forged_and_garbage_handshake_never_admits) {
  ServerEndpoint sv(0x2u);
  Mailboxes mail;
  sv.set_send([&](std::uint64_t a, const Datagram& dg) { mail[a].push_back(dg); });

  // A CHALLENGE_RESPONSE (kind 3) with a bogus server_nonce + cookie, never having seen a
  // CHALLENGE.
  Datagram forged = {3};
  for (int i = 0; i < 8; ++i)
    forged.push_back(0);  // server_nonce = 0
  for (int i = 0; i < 8; ++i)
    forged.push_back(0x5A);  // bogus cookie
  sv.feed(0xC3u, forged, 0.0);
  KCD2S_CHECK(!sv.admitted(0xC3u));

  sv.feed(0xC3u, Datagram{0xFF, 0xFF, 0xFF}, 0.0);  // garbage/unknown kind
  KCD2S_CHECK(!sv.admitted(0xC3u));
  sv.feed(0xC3u, Datagram{}, 0.0);  // empty
  KCD2S_CHECK(!sv.admitted(0xC3u));
  KCD2S_CHECK_EQ(sv.peer_count(), std::size_t{0});
}

KCD2S_TEST(endpoint, two_addresses_get_independent_connections) {
  ServerEndpoint sv(0x33u);
  Mailboxes mail;
  sv.set_send([&](std::uint64_t a, const Datagram& dg) { mail[a].push_back(dg); });

  ClientHandshake c1(0x1111u), c2(0x2222u);
  KCD2S_CHECK(handshake(sv, mail, 0xAAu, c1));
  KCD2S_CHECK(handshake(sv, mail, 0xBBu, c2));
  KCD2S_CHECK_EQ(sv.peer_count(), std::size_t{2});

  Connection* ca = sv.connection(0xAAu);
  Connection* cb = sv.connection(0xBBu);
  KCD2S_CHECK(ca != nullptr && cb != nullptr && ca != cb);

  // A channel packet to AA is delivered on AA's Connection only.
  CaptureLink la;
  Connection sender_a(&la);
  sender_a.queue(static_cast<std::uint8_t>(Channel::Events), Datagram{7});
  sender_a.flush(0.0);
  auto da = sv.feed(0xAAu, la.out.front(), 0.0);
  KCD2S_CHECK_EQ(da.size(), std::size_t{1});
  KCD2S_CHECK(da[0].bytes == (Datagram{7}));

  // BB has received nothing yet.
  CaptureLink lb;
  Connection sender_b(&lb);
  sender_b.queue(static_cast<std::uint8_t>(Channel::Events), Datagram{8});
  sender_b.flush(0.0);
  auto db = sv.feed(0xBBu, lb.out.front(), 0.0);
  KCD2S_CHECK_EQ(db.size(), std::size_t{1});
  KCD2S_CHECK(db[0].bytes == (Datagram{8}));
}

KCD2S_TEST(endpoint, reap_drops_silent_peers) {
  ServerEndpoint sv(0x55u);
  Mailboxes mail;
  sv.set_send([&](std::uint64_t a, const Datagram& dg) { mail[a].push_back(dg); });
  std::vector<std::uint64_t> dropped;
  sv.set_on_drop([&](std::uint64_t a) { dropped.push_back(a); });

  ClientHandshake c1(0x1u), c2(0x2u);
  KCD2S_CHECK(handshake(sv, mail, 0xAAu, c1));  // admitted_at ~ t=0 (handshake uses now=0.0)
  KCD2S_CHECK(handshake(sv, mail, 0xBBu, c2));
  KCD2S_CHECK_EQ(sv.peer_count(), std::size_t{2});

  // AA keeps talking at t=20 (channel packet refreshes its Connection liveness); BB stays silent.
  CaptureLink la;
  Connection sender_a(&la);
  sender_a.queue(static_cast<std::uint8_t>(Channel::Events), Datagram{1});
  sender_a.flush(20.0);
  sv.feed(0xAAu, la.out.front(), 20.0);

  // Reap at t=25 with a 10s threshold: BB (silent since admit ~0) is dropped; AA (recv at 20)
  // stays.
  const std::size_t n = sv.reap_timed_out(25.0, 10.0);
  KCD2S_CHECK_EQ(n, std::size_t{1});
  KCD2S_CHECK_EQ(dropped.size(), std::size_t{1});
  KCD2S_CHECK_EQ(dropped[0], std::uint64_t{0xBBu});
  KCD2S_CHECK(sv.admitted(0xAAu));
  KCD2S_CHECK(!sv.admitted(0xBBu));
  KCD2S_CHECK_EQ(sv.peer_count(), std::size_t{1});

  // A reaped address can handshake again from scratch.
  ClientHandshake c2b(0x3u);
  KCD2S_CHECK(handshake(sv, mail, 0xBBu, c2b));
  KCD2S_CHECK(sv.admitted(0xBBu));
}

KCD2S_TEST(endpoint, rate_limit_drops_burst_then_recovers) {
  ServerEndpoint sv(0x66u);
  Mailboxes mail;
  sv.set_send([&](std::uint64_t a, const Datagram& dg) { mail[a].push_back(dg); });
  sv.set_rate_limit(/*per_sec=*/10.0, /*burst=*/5.0);

  const std::uint64_t addr = 0xF1u;
  const Datagram junk{0xFF};  // a non-handshake datagram (ignored by the gate, but rate-counted)

  // Burst of 8 at the same instant: 5 pass the bucket, 3 dropped.
  for (int i = 0; i < 8; ++i)
    sv.feed(addr, junk, 0.0);
  KCD2S_CHECK_EQ(sv.rate_limited_dropped(), std::uint64_t{3});

  // Immediately more -> all dropped (bucket empty).
  for (int i = 0; i < 4; ++i)
    sv.feed(addr, junk, 0.0);
  KCD2S_CHECK_EQ(sv.rate_limited_dropped(), std::uint64_t{7});

  // 1s later the bucket has refilled by 10 (capped at burst 5): 5 more pass, rest drop.
  for (int i = 0; i < 8; ++i)
    sv.feed(addr, junk, 1.0);
  KCD2S_CHECK_EQ(sv.rate_limited_dropped(), std::uint64_t{10});  // 3 of the 8 dropped
}

KCD2S_TEST(endpoint, rate_limit_is_per_address) {
  ServerEndpoint sv(0x67u);
  Mailboxes mail;
  sv.set_send([&](std::uint64_t a, const Datagram& dg) { mail[a].push_back(dg); });
  sv.set_rate_limit(1.0, 3.0);
  const Datagram junk{0xFF};

  // A floods; B should still get its full burst.
  for (int i = 0; i < 20; ++i)
    sv.feed(0xA0u, junk, 0.0);  // A: 3 pass, 17 dropped
  KCD2S_CHECK_EQ(sv.rate_limited_dropped(), std::uint64_t{17});

  // B sends exactly its burst -> none dropped (flooder A didn't starve B).
  const std::uint64_t before = sv.rate_limited_dropped();
  for (int i = 0; i < 3; ++i)
    sv.feed(0xB0u, junk, 0.0);
  KCD2S_CHECK_EQ(sv.rate_limited_dropped(), before);  // no new drops for B
}

KCD2S_TEST(endpoint, rate_bucket_table_is_bounded) {
  ServerEndpoint sv(0x68u);
  Mailboxes mail;
  sv.set_send([&](std::uint64_t a, const Datagram& dg) { mail[a].push_back(dg); });
  sv.set_rate_limit(5.0, 5.0, /*max_buckets=*/64);
  const Datagram junk{0xFF};

  // Hit 5000 distinct source addresses; the bucket table must stay bounded by max_buckets.
  for (std::uint64_t a = 0; a < 5000; ++a)
    sv.feed(a, junk, static_cast<double>(a) * 0.001);
  KCD2S_CHECK(sv.rate_bucket_count() <= std::size_t{64});
}

KCD2S_TEST(endpoint, no_rate_limit_by_default) {
  ServerEndpoint sv(0x69u);
  Mailboxes mail;
  sv.set_send([&](std::uint64_t a, const Datagram& dg) { mail[a].push_back(dg); });
  const Datagram junk{0xFF};
  for (int i = 0; i < 1000; ++i)
    sv.feed(0xC0u, junk, 0.0);
  KCD2S_CHECK_EQ(sv.rate_limited_dropped(), std::uint64_t{0});  // disabled unless set
  KCD2S_CHECK_EQ(sv.rate_bucket_count(), std::size_t{0});
}

KCD2S_TEST(endpoint, resume_token_rebinds_session) {
  ServerEndpoint sv(0x70u);
  Mailboxes mail;
  sv.set_send([&](std::uint64_t a, const Datagram& dg) { mail[a].push_back(dg); });
  sv.set_resume_grace(30.0);

  ClientHandshake c1(0x1u);
  KCD2S_CHECK(handshake(sv, mail, 0xAAu, c1));
  const auto s1 = sv.session_id(0xAAu);
  const auto t1 = sv.resume_token(0xAAu);
  KCD2S_CHECK(s1 != 0u);

  // AA goes silent and is reaped at t=20 -> its session enters the grace list.
  KCD2S_CHECK_EQ(sv.reap_timed_out(20.0, 10.0), std::size_t{1});
  KCD2S_CHECK(!sv.admitted(0xAAu));

  // AA re-handshakes (fresh session), then presents its old (s1,t1) within grace -> resumes s1.
  ClientHandshake c1b(0x2u);
  KCD2S_CHECK(handshake(sv, mail, 0xAAu, c1b));
  const auto s_fresh = sv.session_id(0xAAu);
  KCD2S_CHECK(s_fresh != s1);

  auto r = sv.try_resume(0xAAu, s1, t1, 25.0);
  KCD2S_CHECK(r.resumed);
  KCD2S_CHECK_EQ(r.session, s1);
  KCD2S_CHECK_EQ(sv.session_id(0xAAu), s1);  // the address is now back on its old identity
}

KCD2S_TEST(endpoint, forged_resume_token_stays_fresh) {
  ServerEndpoint sv(0x71u);
  Mailboxes mail;
  sv.set_send([&](std::uint64_t a, const Datagram& dg) { mail[a].push_back(dg); });

  ClientHandshake c(0x9u);
  KCD2S_CHECK(handshake(sv, mail, 0xBBu, c));
  const auto s_b = sv.session_id(0xBBu);

  auto r = sv.try_resume(0xBBu, /*old_session=*/12345u, /*old_token=*/0xDEADBEEFu, 1.0);
  KCD2S_CHECK(!r.resumed);         // forged token
  KCD2S_CHECK_EQ(r.session, s_b);  // stays on its fresh session
  KCD2S_CHECK_EQ(sv.session_id(0xBBu), s_b);
}

KCD2S_TEST(endpoint, resume_expires_after_grace) {
  ServerEndpoint sv(0x72u);
  Mailboxes mail;
  sv.set_send([&](std::uint64_t a, const Datagram& dg) { mail[a].push_back(dg); });
  sv.set_resume_grace(5.0);

  ClientHandshake c(0x3u);
  KCD2S_CHECK(handshake(sv, mail, 0xCCu, c));
  const auto s_c = sv.session_id(0xCCu);
  const auto t_c = sv.resume_token(0xCCu);

  KCD2S_CHECK_EQ(sv.reap_timed_out(20.0, 10.0), std::size_t{1});  // grace[s_c] = 20
  ClientHandshake c2(0x4u);
  KCD2S_CHECK(handshake(sv, mail, 0xCCu, c2));

  // Valid token, but presented at t=40 (> 5s grace since the t=20 drop) -> no resume, stays fresh.
  auto r = sv.try_resume(0xCCu, s_c, t_c, 40.0);
  KCD2S_CHECK(!r.resumed);
  KCD2S_CHECK(sv.session_id(0xCCu) != s_c);
}

KCD2S_TEST(endpoint, server_flush_sends_to_admitted_peer) {
  ServerEndpoint sv(0x44u);
  Mailboxes mail;
  sv.set_send([&](std::uint64_t a, const Datagram& dg) { mail[a].push_back(dg); });

  const std::uint64_t addr = 0xD4u;
  ClientHandshake ch(0x9u);
  KCD2S_CHECK(handshake(sv, mail, addr, ch));
  mail[addr].clear();  // drop handshake datagrams

  // The server queues a snapshot to the peer and flushes -> a datagram reaches the peer's mailbox.
  sv.connection(addr)->queue(static_cast<std::uint8_t>(Channel::Snapshot), Datagram{5, 5});
  sv.flush(0.0);
  KCD2S_CHECK(!mail[addr].empty());
}
