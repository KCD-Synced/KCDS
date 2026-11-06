// Selective ACK/NAK + RTT, Glenn-Fiedler style.
//
// Each outgoing datagram carries (peerSeq, ackSeq, ackBits): peerSeq is our own sender sequence;
// ackSeq is the latest sequence we have received from the peer; ackBits acknowledges the 32
// sequences preceding ackSeq. Because acks are redundant (every packet re-acks a window), losing an
// ack packet does not stall delivery — the next packet re-acks. This is why snapshots can be
// unreliable yet the server still learns which baselines arrived.
//
// AckSystem owns both directions for one peer:
//   - sender side: allocate sequences, record send times, learn which of our packets were acked
//     (for reliable-message resolution + RTT).
//   - receiver side: record received sequences, generate (ackSeq, ackBits).
#ifndef KCD2S_TRANSPORT_ACK_SYSTEM_HPP
#define KCD2S_TRANSPORT_ACK_SYSTEM_HPP

#include <cstdint>
#include <vector>

#include "kcd2s/transport/sequence_buffer.hpp"

namespace kcd2s::transport {

class AckSystem {
 public:
  // --- sender side ---------------------------------------------------------

  // Allocate the next outgoing sequence number (wraps at 2^16).
  std::uint16_t next_sequence() {
    return sequence_++;
  }

  // Record each sent datagram. Header-only keep-alives do not solicit an immediate response,
  // so their eventual acknowledgements must not inflate the payload RTT estimate.
  void on_packet_sent(std::uint16_t seq, double time, bool measure_rtt = true);

  // Apply an ack received from the peer. Returns the sequences newly acked by this call (each
  // reported at most once across calls). Updates the smoothed RTT estimate from their send times.
  std::vector<std::uint16_t> on_ack_received(std::uint16_t ack_seq, std::uint32_t ack_bits,
                                             double time);

  double rtt_seconds() const {
    return rtt_;
  }

  // --- receiver side -------------------------------------------------------

  // Record that a datagram with `seq` arrived from the peer (duplicates are harmless).
  // Returns true if this sequence had not been seen before (useful for dropping duplicate packets).
  bool on_packet_received(std::uint16_t seq);

  // Produce the ack fields to stamp into our next outgoing datagram. If nothing has been received
  // yet, returns (0, 0) and has_received()==false tells the caller it is a cold ack.
  void generate_ack(std::uint16_t& ack_seq, std::uint32_t& ack_bits) const;
  bool has_received() const {
    return received_.has_latest();
  }

 private:
  struct SentData {
    double time = 0.0;
    bool acked = false;
    bool measure_rtt = true;
  };
  struct RecvData {
    std::uint8_t present = 0;
  };

  void sample_rtt(double rtt_sample);

  std::uint16_t sequence_ = 0;
  SequenceBuffer<SentData> sent_;
  SequenceBuffer<RecvData> received_;
  double rtt_ = 0.0;
  bool rtt_initialized_ = false;
};

}  // namespace kcd2s::transport

#endif  // KCD2S_TRANSPORT_ACK_SYSTEM_HPP
