#include "kcd2s/transport/fragment.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

#include "kcd2s/testkit.hpp"
#include "kcd2s/transport/connection.hpp"
#include "kcd2s/transport/datagram.hpp"
#include "kcd2s/wire/packet.hpp"

using kcd2s::transport::Connection;
using kcd2s::transport::Datagram;
using kcd2s::transport::DeliveryTier;
using kcd2s::transport::FragmentingLink;
using kcd2s::transport::IDatagramLink;
using kcd2s::transport::LinkParams;
using kcd2s::transport::make_loopback_pair;
using kcd2s::transport::make_sim_pair;

namespace {

// A fully controllable in-process link pair for deterministic fragment-loss tests. `drop` decides,
// per outbound datagram (by 0-based send index), whether to silently drop it.
class ControlLink : public IDatagramLink {
 public:
  using IDatagramLink::send;
  void send(const std::uint8_t* data, std::size_t len) override {
    const int idx = sent_++;
    if (drop && drop(idx))
      return;
    if (peer_)
      peer_->inbox.push_back(Datagram(data, data + len));
  }
  bool recv(Datagram& out) override {
    if (inbox.empty())
      return false;
    out = std::move(inbox.front());
    inbox.pop_front();
    return true;
  }
  void poll() override {
  }
  void set_peer(ControlLink* p) {
    peer_ = p;
  }

  std::function<bool(int)> drop;
  std::deque<Datagram> inbox;
  int sent_ = 0;

 private:
  ControlLink* peer_ = nullptr;
};

std::vector<std::uint8_t> pattern(std::size_t n, std::uint8_t seed = 0) {
  std::vector<std::uint8_t> v(n);
  for (std::size_t i = 0; i < n; ++i)
    v[i] = static_cast<std::uint8_t>((i * 31u + seed * 7u + 5u) & 0xFFu);
  return v;
}

}  // namespace

KCD2S_TEST(fragment, small_payload_passes_through_unfragmented) {
  auto [a, b] = make_loopback_pair();
  FragmentingLink fa(a.get(), 64);
  FragmentingLink fb(b.get(), 64);
  const auto msg = pattern(10);
  fa.send(msg);
  KCD2S_CHECK_EQ(fa.fragments_sent(), std::uint64_t{0});  // never fragmented

  Datagram out;
  KCD2S_CHECK(fb.recv(out));
  KCD2S_CHECK(out == msg);
  KCD2S_CHECK(!fb.recv(out));
}

KCD2S_TEST(fragment, large_payload_reassembles_lossless) {
  auto [a, b] = make_loopback_pair();
  FragmentingLink fa(a.get(), 64);
  FragmentingLink fb(b.get(), 64);
  const auto msg = pattern(1000, 3);
  fa.send(msg);
  KCD2S_CHECK(fa.fragments_sent() > 1);  // actually split

  Datagram out;
  KCD2S_CHECK(fb.recv(out));
  KCD2S_CHECK(out == msg);  // byte-for-byte intact
  KCD2S_CHECK(!fb.recv(out));
  KCD2S_CHECK_EQ(fb.partial_count(), std::size_t{0});  // no leftovers
}

KCD2S_TEST(fragment, exact_mtu_boundary) {
  auto [a, b] = make_loopback_pair();
  FragmentingLink fa(a.get(), 64);
  FragmentingLink fb(b.get(), 64);
  // len + 1 (tag) == mtu : still a single unfragmented datagram.
  const auto msg = pattern(63, 1);
  fa.send(msg);
  KCD2S_CHECK_EQ(fa.fragments_sent(), std::uint64_t{0});
  Datagram out;
  KCD2S_CHECK(fb.recv(out));
  KCD2S_CHECK(out == msg);

  // One byte more forces fragmentation.
  const auto msg2 = pattern(64, 2);
  fa.send(msg2);
  KCD2S_CHECK(fa.fragments_sent() > 0);
  KCD2S_CHECK(fb.recv(out));
  KCD2S_CHECK(out == msg2);
}

KCD2S_TEST(fragment, reassembles_regardless_of_fragment_order) {
  ControlLink a, b;
  a.set_peer(&b);
  b.set_peer(&a);
  FragmentingLink fa(&a, 64);
  const auto msg = pattern(600, 9);
  fa.send(msg);
  KCD2S_CHECK(b.inbox.size() > 2);               // several fragments landed in b's inbox
  std::reverse(b.inbox.begin(), b.inbox.end());  // deliver them out of order

  FragmentingLink fb(&b, 64);
  Datagram out;
  KCD2S_CHECK(fb.recv(out));
  KCD2S_CHECK(out == msg);  // index-keyed reassembly ignores arrival order
}

KCD2S_TEST(fragment, large_payload_survives_a_real_connection) {
  // End-to-end: a Connection queues an oversized snapshot-channel message; the FragmentingLink
  // splits the whole datagram and the peer reassembles it before read_packet ever runs.
  auto [a, b] = make_loopback_pair();
  FragmentingLink fa(a.get(), 256);
  FragmentingLink fb(b.get(), 256);
  Connection ca(&fa);
  Connection cb(&fb);

  const auto payload = pattern(5000, 7);
  const std::uint8_t ch = static_cast<std::uint8_t>(kcd2s::wire::Channel::Snapshot);
  ca.queue(ch, payload);
  ca.flush(0.0);  // one logical packet, many datagram fragments

  auto msgs = cb.receive(0.0);
  KCD2S_CHECK_EQ(msgs.size(), std::size_t{1});
  KCD2S_CHECK_EQ(msgs[0].channel, ch);
  KCD2S_CHECK(msgs[0].bytes == payload);
}

KCD2S_TEST(fragment, incomplete_set_dropped_then_full_copy_reassembles) {
  ControlLink a, b;
  a.set_peer(&b);
  b.set_peer(&a);
  FragmentingLink fa(&a, 64);
  FragmentingLink fb(&b, 64);
  const auto msg = pattern(800, 4);

  // First copy: drop the very first fragment -> the set can never complete.
  a.drop = [](int idx) { return idx == 0; };
  fa.send(msg);
  Datagram out;
  KCD2S_CHECK(!fb.recv(out));                          // nothing delivered, no crash
  KCD2S_CHECK_EQ(fb.partial_count(), std::size_t{1});  // one incomplete set held

  // Second copy: lossless -> reassembles cleanly despite the stale partial still parked.
  a.drop = nullptr;
  fa.send(msg);
  KCD2S_CHECK(fb.recv(out));
  KCD2S_CHECK(out == msg);
}

KCD2S_TEST(fragment, incomplete_set_evicted_on_timeout) {
  ControlLink a, b;
  a.set_peer(&b);
  b.set_peer(&a);
  FragmentingLink fa(&a, 64);
  FragmentingLink fb(&b, 64);
  fb.set_reassembly_timeout(3);

  a.drop = [](int idx) { return idx == 1; };  // drop the 2nd fragment
  fa.send(pattern(400, 2));
  Datagram out;
  KCD2S_CHECK(!fb.recv(out));
  KCD2S_CHECK_EQ(fb.partial_count(), std::size_t{1});

  for (int i = 0; i < 4; ++i)
    fb.poll();                                         // past the timeout window
  KCD2S_CHECK_EQ(fb.partial_count(), std::size_t{0});  // swept
}

KCD2S_TEST(fragment, capacity_evicts_oldest_partials) {
  ControlLink a, b;
  a.set_peer(&b);
  b.set_peer(&a);
  FragmentingLink fa(&a, 64);
  FragmentingLink fb(&b, 64);
  fb.set_max_partials(2);

  // Each 100-byte message is 2 fragments (chunk = 64-11 = 53). Drop every 2nd datagram so each
  // message leaves exactly one (incomplete) fragment behind.
  a.drop = [](int idx) { return (idx % 2) == 1; };
  for (int m = 0; m < 5; ++m) {
    fa.send(pattern(100, static_cast<std::uint8_t>(m)));
    Datagram out;
    while (fb.recv(out)) {
    }  // drain -> process fragments (none complete)
  }
  KCD2S_CHECK(fb.partial_count() <= std::size_t{2});  // capped, oldest evicted
}

KCD2S_TEST(fragment, lossy_link_never_corrupts_reassembly) {
  // Real random per-fragment loss: some copies never complete, but every copy that DOES reassemble
  // is byte-identical, and over many sends at least one gets through.
  LinkParams lp;
  lp.loss = 0.1;
  lp.seed = 0xBEEF1234u;
  auto [a, b] = make_sim_pair(lp, LinkParams{});
  FragmentingLink fa(a.get(), 96);
  FragmentingLink fb(b.get(), 96);
  const auto msg = pattern(1200, 5);

  int reassembled = 0;
  for (int i = 0; i < 60; ++i) {
    fa.send(msg);
    Datagram out;
    while (fb.recv(out)) {
      KCD2S_CHECK(out == msg);  // never a corrupt/mismatched reassembly
      ++reassembled;
    }
  }
  KCD2S_CHECK(reassembled > 0);                        // loss didn't wipe out everything
  KCD2S_CHECK(fb.partial_count() <= std::size_t{64});  // bounded, no leak
}

KCD2S_TEST(fragment, garbage_datagram_is_ignored) {
  ControlLink a, b;
  a.set_peer(&b);
  b.set_peer(&a);
  FragmentingLink fb(&b, 64);
  // Hand-crafted junk: a fragment tag with a wild header. Must not crash or deliver.
  b.inbox.push_back(Datagram{1, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 7, 7});
  b.inbox.push_back(Datagram{0});         // unfragmented, empty payload
  b.inbox.push_back(Datagram{});          // empty datagram
  b.inbox.push_back(Datagram{99, 1, 2});  // unknown tag
  Datagram out;
  KCD2S_CHECK(fb.recv(out));  // the tag-0 empty one yields an empty datagram
  KCD2S_CHECK(out.empty());
  KCD2S_CHECK(!fb.recv(out));  // everything else dropped, no crash
}

// ---------------------------------------------------------------------------- verbatim framing
// the UDP host and client use the verbatim framing, so a datagram that fits is sent
// unchanged and only oversized or ambiguous datagrams carry framing.

using kcd2s::transport::FragmentFraming;
using kcd2s::transport::kVerbatimEscapeTag;
using kcd2s::transport::kVerbatimFragmentTag;

KCD2S_TEST(fragment, verbatim_small_datagram_is_sent_unchanged) {
  ControlLink a, b;
  a.set_peer(&b);
  b.set_peer(&a);
  FragmentingLink fa(&a, 64, FragmentFraming::Verbatim);
  FragmentingLink fb(&b, 64, FragmentFraming::Verbatim);
  const auto msg = pattern(64, 1);  // exactly the MTU, first byte is not a reserved tag
  KCD2S_CHECK(msg[0] != kVerbatimEscapeTag && msg[0] != kVerbatimFragmentTag);
  fa.send(msg);
  KCD2S_CHECK_EQ(b.inbox.size(), std::size_t{1});
  KCD2S_CHECK(b.inbox.front() == msg);  // no framing byte on the wire
  KCD2S_CHECK_EQ(fa.fragments_sent(), std::uint64_t{0});

  Datagram out;
  KCD2S_CHECK(fb.recv(out));
  KCD2S_CHECK(out == msg);
}

KCD2S_TEST(fragment, verbatim_escapes_reserved_first_bytes_and_empty_payloads) {
  ControlLink a, b;
  a.set_peer(&b);
  b.set_peer(&a);
  FragmentingLink fa(&a, 64, FragmentFraming::Verbatim);
  FragmentingLink fb(&b, 64, FragmentFraming::Verbatim);
  const std::vector<std::vector<std::uint8_t>> messages{
      {kVerbatimEscapeTag, 1, 2}, {kVerbatimFragmentTag, 3}, {}};
  for (const auto& msg : messages)
    fa.send(msg);
  KCD2S_CHECK_EQ(b.inbox.size(), messages.size());
  for (const auto& frame : b.inbox)
    KCD2S_CHECK_EQ(frame.front(), kVerbatimEscapeTag);
  for (const auto& msg : messages) {
    Datagram out;
    KCD2S_CHECK(fb.recv(out));
    KCD2S_CHECK(out == msg);
  }
}

KCD2S_TEST(fragment, verbatim_fragments_never_exceed_the_mtu) {
  ControlLink a, b;
  a.set_peer(&b);
  b.set_peer(&a);
  constexpr std::size_t kMtu = 1200;
  FragmentingLink fa(&a, kMtu, FragmentFraming::Verbatim);
  FragmentingLink fb(&b, kMtu, FragmentFraming::Verbatim);
  for (std::size_t size : {std::size_t{1201}, std::size_t{4096}, std::size_t{65000}}) {
    const auto msg = pattern(size, static_cast<std::uint8_t>(size));
    fa.send(msg);
    for (const auto& frame : b.inbox) {
      KCD2S_CHECK(frame.size() <= kMtu);
      KCD2S_CHECK_EQ(frame.front(), kVerbatimFragmentTag);
    }
    Datagram out;
    KCD2S_CHECK(fb.recv(out));
    KCD2S_CHECK(out == msg);
  }
  KCD2S_CHECK(fa.largest_frame() <= kMtu);
  KCD2S_CHECK_EQ(fa.fragmented_datagrams(), std::uint64_t{3});
}

KCD2S_TEST(fragment, verbatim_rejects_sets_above_the_message_limit) {
  ControlLink a, b;
  a.set_peer(&b);
  b.set_peer(&a);
  FragmentingLink fa(&a, 128, FragmentFraming::Verbatim);
  FragmentingLink fb(&b, 128, FragmentFraming::Verbatim);
  fb.set_max_message_bytes(1000);
  fa.send(pattern(1001, 4));
  Datagram out;
  KCD2S_CHECK(!fb.recv(out));
  KCD2S_CHECK_EQ(fb.partial_count(), std::size_t{0});  // nothing buffered for it
  fa.send(pattern(1000, 5));
  KCD2S_CHECK(fb.recv(out));
  KCD2S_CHECK_EQ(out.size(), std::size_t{1000});
}

KCD2S_TEST(fragment, verbatim_chunks_beyond_announced_total_are_dropped) {
  ControlLink b;
  FragmentingLink fb(&b, 64, FragmentFraming::Verbatim);
  // Two fragments of a set that announces 10 bytes in total but carries 2 x 8 bytes.
  for (std::uint8_t index = 0; index < 2; ++index) {
    Datagram frame{kVerbatimFragmentTag, 7, 0, index, 0, 2, 0, 10, 0, 0, 0};
    frame.insert(frame.end(), 8, 0x55);
    b.inbox.push_back(frame);
  }
  Datagram out;
  KCD2S_CHECK(!fb.recv(out));
  KCD2S_CHECK_EQ(fb.partial_count(), std::size_t{0});
}

KCD2S_TEST(fragment, verbatim_lossy_link_never_loses_reliable_messages) {
  // Large unreliable payloads are fragmented and some are lost; every reliable message still
  // arrives exactly once and in order.
  LinkParams lossy;
  lossy.loss = 0.05;
  lossy.seed = 0x1234ABCDu;
  auto [a, b] = make_sim_pair(lossy, lossy);
  FragmentingLink fa(a.get(), 1200, FragmentFraming::Verbatim);
  FragmentingLink fb(b.get(), 1200, FragmentFraming::Verbatim);
  Connection ca(&fa);
  Connection cb(&fb);
  const std::uint8_t snapshot = static_cast<std::uint8_t>(kcd2s::wire::Channel::Snapshot);
  const std::uint8_t events = static_cast<std::uint8_t>(kcd2s::wire::Channel::Events);

  std::vector<std::uint8_t> received;
  double now = 0.0;
  for (int tick = 0; tick < 400; ++tick) {
    now += 1.0 / 60.0;
    ca.queue(snapshot, pattern(6000, static_cast<std::uint8_t>(tick)));
    if (tick < 100)
      ca.queue(events, std::vector<std::uint8_t>{static_cast<std::uint8_t>(tick)});
    ca.flush(now);
    cb.flush(now);  // acknowledgements
    a->poll();
    b->poll();
    for (const auto& message : cb.receive(now)) {
      if (message.channel == events)
        received.push_back(message.bytes.at(0));
    }
    (void)ca.receive(now);
  }
  KCD2S_CHECK_EQ(received.size(), std::size_t{100});
  bool in_order = received.size() == 100;
  for (std::size_t i = 0; i < received.size(); ++i) {
    if (received[i] != static_cast<std::uint8_t>(i))
      in_order = false;
  }
  KCD2S_CHECK(in_order);
  KCD2S_CHECK_EQ(ca.outstanding_reliable(), std::size_t{0});
}
