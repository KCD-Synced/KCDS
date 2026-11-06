#include "kcd2s/transport/ack_system.hpp"

#include "kcd2s/core/seq.hpp"

namespace kcd2s::transport {

namespace {
constexpr double kRttSmoothing = 0.1;  // EWMA weight for new RTT samples
}

void AckSystem::on_packet_sent(std::uint16_t seq, double time, bool measure_rtt) {
  sent_.insert(seq, SentData{time, /*acked=*/false, measure_rtt});
}

bool AckSystem::on_packet_received(std::uint16_t seq) {
  const bool is_new = !received_.exists(seq);
  received_.insert(seq, RecvData{1});
  return is_new;
}

void AckSystem::generate_ack(std::uint16_t& ack_seq, std::uint32_t& ack_bits) const {
  if (!received_.has_latest()) {
    ack_seq = 0;
    ack_bits = 0;
    return;
  }
  ack_seq = received_.latest();
  std::uint32_t bits = 0;
  for (std::uint32_t i = 0; i < 32; ++i) {
    // Bit i acknowledges (ack_seq - 1 - i).
    const std::uint16_t s = static_cast<std::uint16_t>(ack_seq - 1 - i);
    if (received_.exists(s))
      bits |= (1u << i);
  }
  ack_bits = bits;
}

std::vector<std::uint16_t> AckSystem::on_ack_received(std::uint16_t ack_seq, std::uint32_t ack_bits,
                                                      double time) {
  std::vector<std::uint16_t> newly;
  // ack_seq itself, then the 32 preceding sequences flagged in ack_bits.
  for (int i = -1; i < 32; ++i) {
    bool flagged;
    std::uint16_t seq;
    if (i < 0) {
      flagged = true;  // ack_seq is always acknowledged
      seq = ack_seq;
    } else {
      flagged = (ack_bits & (1u << static_cast<std::uint32_t>(i))) != 0;
      seq = static_cast<std::uint16_t>(ack_seq - 1 - static_cast<std::uint16_t>(i));
    }
    if (!flagged)
      continue;
    SentData* s = sent_.find(seq);
    if (s && !s->acked) {
      s->acked = true;
      newly.push_back(seq);
      const double sample = time - s->time;
      if (s->measure_rtt && sample >= 0.0)
        sample_rtt(sample);
    }
  }
  return newly;
}

void AckSystem::sample_rtt(double rtt_sample) {
  if (!rtt_initialized_) {
    rtt_ = rtt_sample;
    rtt_initialized_ = true;
  } else {
    rtt_ += (rtt_sample - rtt_) * kRttSmoothing;
  }
}

}  // namespace kcd2s::transport
