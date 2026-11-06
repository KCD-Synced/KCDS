#include "kcd2s/wire/packet.hpp"

#include "kcd2s/core/bits.hpp"

namespace kcd2s::wire {

// Byte-aligned write_bits(v, 8/16/32) emits least-significant byte first == little-endian, which is
// the KSTP wire order. write_bytes aligns to a byte boundary first (always aligned here anyway).
std::vector<std::uint8_t> write_packet(const Packet& p) {
  core::BitWriter w;
  w.write_bits(p.protocolId, 16);
  w.write_bits(p.peerSeq, 16);
  w.write_bits(p.ackSeq, 16);
  w.write_bits(p.ackBits, 32);
  w.write_bits(p.flags, 8);
  w.write_bits(p.channels.size(), 8);
  for (const auto& ch : p.channels) {
    w.write_bits(ch.channel, 8);
    w.write_bits(ch.bytes.size(), 16);
    if (!ch.bytes.empty())
      w.write_bytes(ch.bytes.data(), ch.bytes.size());
  }
  return w.bytes();
}

bool read_packet(const std::uint8_t* data, std::size_t len, Packet& out) {
  core::BitReader r(data, len);
  out = Packet{};

  out.protocolId = static_cast<std::uint16_t>(r.read_bits(16));
  out.peerSeq = static_cast<std::uint16_t>(r.read_bits(16));
  out.ackSeq = static_cast<std::uint16_t>(r.read_bits(16));
  out.ackBits = static_cast<std::uint32_t>(r.read_bits(32));
  out.flags = static_cast<std::uint8_t>(r.read_bits(8));
  const std::uint64_t channel_count = r.read_bits(8);
  if (r.overrun())
    return false;
  if (channel_count > kMaxChannelsPerPacket)
    return false;

  out.channels.reserve(static_cast<std::size_t>(channel_count));
  for (std::uint64_t i = 0; i < channel_count; ++i) {
    ChannelPayload cp;
    cp.channel = static_cast<std::uint8_t>(r.read_bits(8));
    const std::uint64_t payload_len = r.read_bits(16);
    if (r.overrun())
      return false;
    // Reject a length that would read past the datagram.
    if (payload_len * 8 > r.bits_remaining())
      return false;
    cp.bytes.resize(static_cast<std::size_t>(payload_len));
    r.read_byte_run(cp.bytes.data(), cp.bytes.size());
    if (r.overrun())
      return false;
    out.channels.push_back(std::move(cp));
  }
  return true;
}

}  // namespace kcd2s::wire
