// KSTP datagram framing.
//
// Every datagram begins with a fixed header, then a coalesced list of per-channel payloads:
//
//   u16 protocolId   magic+version; reject mismatched builds early
//   u16 peerSeq      sender sequence (RTT + ack bitfield)
//   u16 ackSeq       last seq seen from the other side
//   u32 ackBits      the 32 seqs preceding ackSeq (Glenn-Fiedler ack)
//   u8  flags        bit0 = ackValid (ackSeq/ackBits are meaningful; clear on a cold start before
//                    the sender has received anything, so a (0,0) ack is not mistaken for acking
//                    packet 0). Remaining bits reserved, sent as 0.
//   u8  channelCount
//     per channel: u8 channelId, u16 payloadLen, bytes[payloadLen]
//
// Little-endian on the wire. read_packet treats its input as untrusted and rejects malformed data.
#ifndef KCD2S_WIRE_PACKET_HPP
#define KCD2S_WIRE_PACKET_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kcd2s::wire {

// Logical channels multiplexed on one KSTP connection (table).
enum class Channel : std::uint8_t {
  Control = 0,   // reliable-ordered: handshake, auth, join, disconnect
  Snapshot = 1,  // unreliable + ack ring: per-tick entity delta snapshots
  Events = 2,    // reliable-ordered: RPCs, chat, resource refresh, reliable state-bag ops
  Upstream = 3,  // unreliable + ack: owner input / owned-entity state (client->server)
  Voice = 4,     // unreliable-sequenced: voice frames
};

// Magic 'KS' (0x4B 0x53) in the high byte pair + protocol version in the low nibble.
// Bump the version when the wire format changes incompatibly.
// Version 2: datagrams above the safe UDP payload are split into verbatim-framed
// fragments (Backend/transport/fragment.hpp) by the UDP host and client. A version 1 peer cannot
// reassemble them, so mismatched builds are kept apart at the handshake.
// Version 3: native animation fields can contain KSA2/KSA3 sparse payloads. Older
// readers do not understand them, so reject those peers before admitting any replicated state.
constexpr std::uint16_t kProtocolVersion = 3;
constexpr std::uint16_t kProtocolId = static_cast<std::uint16_t>(0x4B50u | kProtocolVersion);

// Defensive bound: a single datagram cannot carry more channel blocks than this.
constexpr std::size_t kMaxChannelsPerPacket = 32;

// Header flag bits.
constexpr std::uint8_t kFlagAckValid = 0x01;

struct ChannelPayload {
  std::uint8_t channel = 0;
  std::vector<std::uint8_t> bytes;
};

struct Packet {
  std::uint16_t protocolId = kProtocolId;
  std::uint16_t peerSeq = 0;
  std::uint16_t ackSeq = 0;
  std::uint32_t ackBits = 0;
  std::uint8_t flags = 0;
  std::vector<ChannelPayload> channels;
};

// Serialize a packet to a byte buffer.
std::vector<std::uint8_t> write_packet(const Packet& p);

// Parse a datagram. Returns false (and leaves out unspecified) on any malformed/truncated input,
// too many channels, or a payload length that runs past the buffer. Does NOT validate protocolId
// (the caller decides how to treat a version mismatch); it is parsed into out.protocolId.
bool read_packet(const std::uint8_t* data, std::size_t len, Packet& out);

inline bool read_packet(const std::vector<std::uint8_t>& buf, Packet& out) {
  return read_packet(buf.data(), buf.size(), out);
}

}  // namespace kcd2s::wire

#endif  // KCD2S_WIRE_PACKET_HPP
