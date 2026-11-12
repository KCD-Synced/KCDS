// KSTP connection: per-message delivery tiers over one datagram link.
//
// A Connection owns the header sequence/ack bookkeeping (AckSystem) and the per-channel message
// queues. Callers queue() messages on logical channels; flush() coalesces everything due into ONE
// datagram (one header amortized over the tick's traffic) when needed; receive() drains the link,
// processes acks, and returns inbound messages already filtered/ordered per each channel's tier.
//
// Delivery tiers (assigned per channel; default map matches the §2 channel table):
//   Unreliable           - send once, deliver on arrival, no dedup.
//   UnreliableSequenced  - send once, deliver only if newer than the last delivered (drop old).
//   ReliableUnordered    - retransmit until acked, dedup, deliver in arrival order.
//   ReliableOrdered      - retransmit until acked, deliver strictly in message-sequence order.
//
// Reliability is resend-until-acked, but RTT-timed: an outstanding reliable message is only
// retransmitted once it has gone unacked for an RTO (derived from the AckSystem's smoothed RTT,
// clamped to [min,max]), not blindly on every flush — so a well-acked link stops spamming resends
// while a lossy one still recovers. Pacing is a later refinement. MTU fragmentation lives one layer
// down: wrap the link in a FragmentingLink (Backend/transport/fragment.hpp) so an oversized
// datagram is split/reassembled transparently below here. The connection handshake (protocol gate +
// cookie) is a separate slice; for now a packet whose protocolId mismatches is dropped.
#ifndef KCD2S_TRANSPORT_CONNECTION_HPP
#define KCD2S_TRANSPORT_CONNECTION_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

#include "kcd2s/transport/ack_system.hpp"
#include "kcd2s/transport/datagram.hpp"
#include "kcd2s/transport/sequence_buffer.hpp"
#include "kcd2s/wire/packet.hpp"

namespace kcd2s::transport {

enum class DeliveryTier : std::uint8_t {
  Unreliable = 0,
  UnreliableSequenced = 1,
  ReliableUnordered = 2,
  ReliableOrdered = 3,
};

struct DeliveredMessage {
  std::uint8_t channel = 0;
  std::vector<std::uint8_t> bytes;
};

class Connection {
 public:
  explicit Connection(IDatagramLink* link);

  // Override the delivery tier for a channel (defaults follow the §2 table).
  void set_channel_tier(std::uint8_t channel, DeliveryTier tier);
  // Bound one channel's coalesced block. Zero preserves the existing unlimited
  // behavior; messages larger than a configured budget must be split by the caller.
  // The most of one channel's messages a packet carries when the channel has no budget of its own.
  // The packet writes a channel's payload length in 16 bits, so a burst of more than 64 KB of
  // messages on one channel would have made a packet no one can read, sent again and again (issue
  // 600 props at once stopped a client's control channel). The rest goes in the next packets;
  // a single larger message still goes alone.
  static constexpr std::size_t kMaxChannelPayloadBytes = 16384;

  void set_channel_payload_budget(std::uint8_t channel, std::size_t bytes) {
    channel_budget_[channel] = bytes;
  }

  DeliveryTier channel_tier(std::uint8_t channel) const;

  // Queue a message for transmission on the given channel. Copied; sent on the next flush().
  void queue(std::uint8_t channel, const std::uint8_t* data, std::size_t len);
  void queue(std::uint8_t channel, const std::vector<std::uint8_t>& v) {
    queue(channel, v.data(), v.size());
  }

  // Send due payloads/retransmits immediately. Without a payload, send a fresh transport ACK,
  // the first keep-alive, or a keep-alive after 250 ms of silence. Header-only packets do not
  // elicit another ACK. Suppressed flushes consume no packet sequence or sent-history entry.
  void flush(double now);

  // Drain all available inbound datagrams; return delivered messages in per-channel tier order.
  std::vector<DeliveredMessage> receive(double now);

  double rtt_seconds() const {
    return ack_.rtt_seconds();
  }
  std::size_t outstanding_reliable() const;

  // Liveness (KSTP §2 keep-alive/timeout): the wall time of the most recently received datagram.
  // A quiet peer sends a keep-alive at least every 250 ms while ticked; a crashed/vanished one
  // goes silent. Negative until the first datagram arrives.
  double last_recv_time() const {
    return last_recv_;
  }
  bool has_received_datagram() const {
    return last_recv_ >= 0.0;
  }
  // Seconds since the last received datagram, or -1 if none has ever arrived.
  double seconds_since_recv(double now) const {
    return has_received_datagram() ? now - last_recv_ : -1.0;
  }
  // True once a datagram has been received AND it is older than `threshold` seconds.
  bool is_timed_out(double now, double threshold) const {
    return has_received_datagram() && (now - last_recv_) > threshold;
  }

  // Retransmit tuning: RTO = clamp(rtt * mult, min_rto, max_rto). Defaults: 2x RTT, floored at
  // 50ms, capped at 1s. A reliable message is (re)sent only when it has been outstanding at least
  // one RTO.
  void set_retransmit_params(double rtt_mult, double min_rto, double max_rto) {
    rtt_mult_ = rtt_mult;
    min_rto_ = min_rto;
    max_rto_ = max_rto;
  }
  // Current retransmit timeout in seconds (for tests / instrumentation).
  double retransmit_timeout() const {
    double r = ack_.rtt_seconds() * rtt_mult_;
    if (r < min_rto_)
      r = min_rto_;
    if (r > max_rto_)
      r = max_rto_;
    return r;
  }
  // Cumulative count of reliable message copies written to the wire (first sends + retransmits).
  std::uint64_t reliable_sent_count() const {
    return reliable_sent_;
  }
  // Cumulative count of reliable message copies written again after their first send.
  std::uint64_t reliable_retransmit_count() const {
    return reliable_retransmits_;
  }

  // What this connection wrote to the wire, by kind (sent_kind): message bytes with their 4-byte
  // message header, first sends and resends apart.
  struct SentKind {
    std::uint64_t messages = 0;
    std::uint64_t bytes = 0;
    std::uint64_t resent_bytes = 0;
  };
  // A kind is the channel and, on channels whose messages start with their type byte, that byte
  // (0 on the others: snapshots, owner input, voice, acknowledgements).
  static std::uint16_t sent_kind(std::uint8_t channel, const std::vector<std::uint8_t>& message) {
    const bool typed =
        channel != 1 && channel != 3 && channel != 4 && channel != 5 && channel != 11;
    const std::uint8_t type = typed && !message.empty() ? message[0] : 0;
    return static_cast<std::uint16_t>((channel << 8) | type);
  }
  const std::unordered_map<std::uint16_t, SentKind>& sent_by_kind() const {
    return sent_kinds_;
  }

 private:
  struct OutMsg {
    std::uint16_t seq;
    std::vector<std::uint8_t> bytes;
    double last_send = -1.0;  // seconds; <0 = never sent yet (send on the next flush)
  };
  struct ChannelOut {
    std::uint16_t next_seq = 0;
    std::vector<OutMsg> pending;      // unreliable/sequenced: sent once then cleared
    std::vector<OutMsg> outstanding;  // reliable: resent until acked
  };
  struct ChannelIn {
    // ReliableOrdered
    bool has_expected = false;
    std::uint16_t expected = 0;
    std::map<std::uint16_t, std::vector<std::uint8_t>> reorder;
    // ReliableUnordered dedup
    SequenceBuffer<std::uint8_t> seen;
    // UnreliableSequenced
    bool has_last = false;
    std::uint16_t last = 0;
  };

  ChannelOut& out(std::uint8_t ch) {
    return out_[ch];
  }
  ChannelIn& in(std::uint8_t ch) {
    return in_[ch];
  }

  void deliver_inbound(std::uint8_t channel, std::uint16_t seq, std::vector<std::uint8_t>&& bytes,
                       std::vector<DeliveredMessage>& out_msgs);
  void resolve_acked_packet(std::uint16_t packet_seq);

  IDatagramLink* link_;
  AckSystem ack_;
  std::size_t channel_budget_[256]{};
  DeliveryTier tier_[256];
  std::unordered_map<std::uint8_t, ChannelOut> out_;
  std::unordered_map<std::uint8_t, ChannelIn> in_;
  // Which reliable (channel, msgSeq) pairs rode in each sent packet, for ack resolution.
  std::unordered_map<std::uint16_t, std::vector<std::pair<std::uint8_t, std::uint16_t>>>
      packet_msgs_;

  // Retransmit tuning + instrumentation.
  double rtt_mult_ = 2.0;
  double min_rto_ = 0.05;
  double max_rto_ = 1.0;
  std::uint64_t reliable_sent_ = 0;
  std::uint64_t reliable_retransmits_ = 0;
  std::unordered_map<std::uint16_t, SentKind> sent_kinds_;
  double last_recv_ = -1.0;  // wall time of the last received datagram (liveness)
  double last_send_ = -1.0;
  bool ack_pending_ = false;
  static constexpr double kIdleKeepaliveSeconds = 0.25;
};

}  // namespace kcd2s::transport

#endif  // KCD2S_TRANSPORT_CONNECTION_HPP
