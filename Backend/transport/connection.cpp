#include "kcd2s/transport/connection.hpp"

#include <utility>

#include "kcd2s/core/bits.hpp"
#include "kcd2s/core/seq.hpp"

namespace kcd2s::transport {

using core::BitReader;
using core::BitWriter;
using wire::Channel;

namespace {
bool is_reliable(DeliveryTier t) {
  return t == DeliveryTier::ReliableUnordered || t == DeliveryTier::ReliableOrdered;
}
}  // namespace

Connection::Connection(IDatagramLink* link) : link_(link) {
  for (int i = 0; i < 256; ++i)
    tier_[i] = DeliveryTier::Unreliable;
  // Defaults per the channel table.
  tier_[static_cast<std::uint8_t>(Channel::Control)] = DeliveryTier::ReliableOrdered;
  tier_[static_cast<std::uint8_t>(Channel::Snapshot)] = DeliveryTier::Unreliable;
  tier_[static_cast<std::uint8_t>(Channel::Events)] = DeliveryTier::ReliableOrdered;
  tier_[static_cast<std::uint8_t>(Channel::Upstream)] = DeliveryTier::Unreliable;
  tier_[static_cast<std::uint8_t>(Channel::Voice)] = DeliveryTier::UnreliableSequenced;
}

void Connection::set_channel_tier(std::uint8_t channel, DeliveryTier tier) {
  tier_[channel] = tier;
}
DeliveryTier Connection::channel_tier(std::uint8_t channel) const {
  return tier_[channel];
}

void Connection::queue(std::uint8_t channel, const std::uint8_t* data, std::size_t len) {
  ChannelOut& co = out(channel);
  OutMsg m;
  m.seq = co.next_seq++;
  m.bytes.assign(data, data + len);
  if (is_reliable(tier_[channel]))
    co.outstanding.push_back(std::move(m));
  else
    co.pending.push_back(std::move(m));
}

void Connection::flush(double now) {
  wire::Packet p;
  p.protocolId = wire::kProtocolId;
  std::vector<std::pair<std::uint8_t, std::uint16_t>> mapping;
  const double rto = retransmit_timeout();

  for (auto& [ch, co] : out_) {
    const bool reliable = is_reliable(tier_[ch]);
    std::vector<OutMsg>& src = reliable ? co.outstanding : co.pending;
    if (src.empty())
      continue;

    // Unreliable: everything pending ships once. Reliable: only messages never sent, or whose last
    // send is at least one RTO old, are (re)transmitted this flush — a still-fresh unacked message
    // waits rather than riding every datagram.
    std::vector<OutMsg*> due;
    due.reserve(src.size());
    std::size_t payload_bytes = 2;
    for (auto& m : src) {
      if (!reliable || m.last_send < 0.0 || (now - m.last_send) >= rto) {
        const auto required = m.bytes.size() + 4;
        const bool own_budget = channel_budget_[ch] != 0;
        const std::size_t limit = own_budget ? channel_budget_[ch] : kMaxChannelPayloadBytes;
        const bool first = due.empty();
        if (payload_bytes + required > limit && (own_budget || !first))
          break;
        due.push_back(&m);
        payload_bytes += required;
      }
    }
    if (due.empty())
      continue;

    BitWriter w;
    w.write_bits(due.size(), 16);
    for (OutMsg* m : due) {
      w.write_bits(m->seq, 16);
      w.write_bits(m->bytes.size(), 16);
      if (!m->bytes.empty())
        w.write_bytes(m->bytes.data(), m->bytes.size());
      SentKind& kind = sent_kinds_[sent_kind(ch, m->bytes)];
      if (m->last_send >= 0.0) {
        kind.resent_bytes += m->bytes.size() + 4;
      } else {
        kind.messages += 1;
        kind.bytes += m->bytes.size() + 4;
      }
      if (reliable) {
        mapping.emplace_back(ch, m->seq);
        ++reliable_sent_;  // every reliable copy written: first sends + retransmits
        if (m->last_send >= 0.0) {
          ++reliable_retransmits_;
        }
      }
      m->last_send = now;
    }
    wire::ChannelPayload cp;
    cp.channel = ch;
    cp.bytes = w.bytes();
    p.channels.push_back(std::move(cp));

    if (!reliable) {
      // Selected unreliable messages ship once. Budget-deferred messages keep
      // their place rather than disappearing before they have been sent.
      src.erase(src.begin(), src.begin() + static_cast<std::ptrdiff_t>(due.size()));
    }
  }

  const bool has_payload = !p.channels.empty();
  if (!has_payload && !ack_pending_ && last_send_ >= 0.0 && now >= last_send_ &&
      now - last_send_ < kIdleKeepaliveSeconds) {
    return;
  }
  // Sequence/history are allocated only for actual sends. A header-only packet's peer may
  // stay quiet until its own keep-alive, so only payload-bearing packets measure RTT.
  const std::uint16_t seq = ack_.next_sequence();
  ack_.on_packet_sent(seq, now, has_payload);
  p.peerSeq = seq;
  ack_.generate_ack(p.ackSeq, p.ackBits);
  if (ack_.has_received()) {
    p.flags |= wire::kFlagAckValid;
  }
  if (mapping.empty()) {
    packet_msgs_.erase(seq);
  } else {
    packet_msgs_[seq] = std::move(mapping);
  }

  const std::vector<std::uint8_t> datagram = wire::write_packet(p);
  link_->send(datagram.data(), datagram.size());
  last_send_ = now;
  ack_pending_ = false;
}

void Connection::resolve_acked_packet(std::uint16_t packet_seq) {
  auto it = packet_msgs_.find(packet_seq);
  if (it == packet_msgs_.end())
    return;
  for (const auto& [ch, msg_seq] : it->second) {
    ChannelOut& co = out(ch);
    for (std::size_t i = 0; i < co.outstanding.size(); ++i) {
      if (co.outstanding[i].seq == msg_seq) {
        co.outstanding.erase(co.outstanding.begin() + static_cast<std::ptrdiff_t>(i));
        break;
      }
    }
  }
  packet_msgs_.erase(it);
}

void Connection::deliver_inbound(std::uint8_t channel, std::uint16_t seq,
                                 std::vector<std::uint8_t>&& bytes,
                                 std::vector<DeliveredMessage>& out_msgs) {
  const DeliveryTier t = tier_[channel];
  ChannelIn& ci = in(channel);

  switch (t) {
    case DeliveryTier::Unreliable:
      out_msgs.push_back(DeliveredMessage{channel, std::move(bytes)});
      break;

    case DeliveryTier::UnreliableSequenced:
      if (!ci.has_last || core::seq_greater(seq, ci.last)) {
        ci.has_last = true;
        ci.last = seq;
        out_msgs.push_back(DeliveredMessage{channel, std::move(bytes)});
      }
      break;

    case DeliveryTier::ReliableUnordered:
      if (!ci.seen.exists(seq)) {
        ci.seen.insert(seq, 1);
        out_msgs.push_back(DeliveredMessage{channel, std::move(bytes)});
      }
      break;

    case DeliveryTier::ReliableOrdered: {
      if (!ci.has_expected) {
        ci.has_expected = true;
        ci.expected = 0;  // outbound msg seqs start at 0
      }
      if (core::seq_greater(ci.expected, seq))
        break;  // already delivered (dup/old)
      if (seq == ci.expected) {
        out_msgs.push_back(DeliveredMessage{channel, std::move(bytes)});
        ci.expected = static_cast<std::uint16_t>(ci.expected + 1);
        // Drain any buffered successors now in order.
        auto next = ci.reorder.find(ci.expected);
        while (next != ci.reorder.end()) {
          out_msgs.push_back(DeliveredMessage{channel, std::move(next->second)});
          ci.reorder.erase(next);
          ci.expected = static_cast<std::uint16_t>(ci.expected + 1);
          next = ci.reorder.find(ci.expected);
        }
      } else {
        ci.reorder.emplace(seq, std::move(bytes));  // out of order: hold
      }
      break;
    }
  }
}

std::vector<DeliveredMessage> Connection::receive(double now) {
  std::vector<DeliveredMessage> delivered;
  Datagram raw;
  while (link_->recv(raw)) {
    last_recv_ = now;  // any inbound datagram is proof of life (keep-alive)
    wire::Packet p;
    if (!wire::read_packet(raw, p))
      continue;  // malformed
    if (p.protocolId != wire::kProtocolId)
      continue;  // protocol gate

    const bool is_new = ack_.on_packet_received(p.peerSeq);
    // ACK payload-bearing packets, including duplicates whose earlier ACK may have been lost.
    // Header-only ACKs/keep-alives refresh liveness and receipt history without starting ping-pong.
    if (!p.channels.empty()) {
      ack_pending_ = true;
    }
    if (p.flags & wire::kFlagAckValid) {
      for (std::uint16_t s : ack_.on_ack_received(p.ackSeq, p.ackBits, now))
        resolve_acked_packet(s);
    }
    if (!is_new)
      continue;  // duplicate datagram: acks handled, skip message delivery

    for (auto& cp : p.channels) {
      BitReader r(cp.bytes);
      const std::uint64_t count = r.read_bits(16);
      if (r.overrun())
        continue;
      for (std::uint64_t i = 0; i < count; ++i) {
        const std::uint16_t mseq = static_cast<std::uint16_t>(r.read_bits(16));
        const std::uint64_t len = r.read_bits(16);
        if (r.overrun())
          break;
        if (len * 8 > r.bits_remaining())
          break;
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(len));
        r.read_byte_run(bytes.data(), bytes.size());
        if (r.overrun())
          break;
        deliver_inbound(cp.channel, mseq, std::move(bytes), delivered);
      }
    }
  }
  return delivered;
}

std::size_t Connection::outstanding_reliable() const {
  std::size_t n = 0;
  for (const auto& [ch, co] : out_)
    n += co.outstanding.size();
  return n;
}

}  // namespace kcd2s::transport
