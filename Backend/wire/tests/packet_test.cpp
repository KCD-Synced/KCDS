#include "kcd2s/wire/packet.hpp"

#include <cstdint>

#include "kcd2s/testkit.hpp"

using kcd2s::wire::Channel;
using kcd2s::wire::Packet;
using kcd2s::wire::read_packet;
using kcd2s::wire::write_packet;

static kcd2s::wire::ChannelPayload make_ch(Channel c, std::initializer_list<std::uint8_t> b) {
  kcd2s::wire::ChannelPayload cp;
  cp.channel = static_cast<std::uint8_t>(c);
  cp.bytes.assign(b);
  return cp;
}

KCD2S_TEST(packet, header_roundtrip_no_channels) {
  Packet p;
  p.peerSeq = 0x1234;
  p.ackSeq = 0xBEEF;
  p.ackBits = 0xDEADC0DE;
  auto buf = write_packet(p);

  Packet got;
  KCD2S_CHECK(read_packet(buf, got));
  KCD2S_CHECK_EQ(got.protocolId, kcd2s::wire::kProtocolId);
  KCD2S_CHECK_EQ(got.peerSeq, std::uint16_t{0x1234});
  KCD2S_CHECK_EQ(got.ackSeq, std::uint16_t{0xBEEF});
  KCD2S_CHECK_EQ(got.ackBits, std::uint32_t{0xDEADC0DE});
  KCD2S_CHECK_EQ(got.channels.size(), std::size_t{0});
}

KCD2S_TEST(packet, multi_channel_roundtrip) {
  Packet p;
  p.peerSeq = 7;
  p.channels.push_back(make_ch(Channel::Control, {0x01, 0x02, 0x03}));
  p.channels.push_back(make_ch(Channel::Snapshot, {0xAA, 0xBB}));
  p.channels.push_back(make_ch(Channel::Events, {}));  // empty payload is valid

  auto buf = write_packet(p);
  Packet got;
  KCD2S_CHECK(read_packet(buf, got));
  KCD2S_CHECK_EQ(got.channels.size(), std::size_t{3});
  KCD2S_CHECK_EQ(got.channels[0].channel, static_cast<std::uint8_t>(Channel::Control));
  KCD2S_CHECK_EQ(got.channels[0].bytes.size(), std::size_t{3});
  KCD2S_CHECK_EQ(got.channels[0].bytes[2], std::uint8_t{0x03});
  KCD2S_CHECK_EQ(got.channels[1].bytes[0], std::uint8_t{0xAA});
  KCD2S_CHECK_EQ(got.channels[2].bytes.size(), std::size_t{0});
}

KCD2S_TEST(packet, little_endian_on_wire) {
  Packet p;
  p.peerSeq = 0x00FF;  // low byte 0xFF, high byte 0x00
  auto buf = write_packet(p);
  // protocolId occupies bytes [0,1]; peerSeq bytes [2,3] little-endian => 0xFF then 0x00.
  KCD2S_CHECK_EQ(buf[2], std::uint8_t{0xFF});
  KCD2S_CHECK_EQ(buf[3], std::uint8_t{0x00});
}

KCD2S_TEST(packet, reject_truncated_header) {
  Packet p;
  p.channels.push_back(make_ch(Channel::Control, {1, 2, 3}));
  auto buf = write_packet(p);
  buf.resize(5);  // chop mid-header
  Packet got;
  KCD2S_CHECK(!read_packet(buf, got));
}

KCD2S_TEST(packet, reject_payload_len_past_end) {
  Packet p;
  p.channels.push_back(make_ch(Channel::Control, {1, 2, 3, 4}));
  auto buf = write_packet(p);
  buf.pop_back();  // claim 4 payload bytes but only 3 remain
  Packet got;
  KCD2S_CHECK(!read_packet(buf, got));
}

KCD2S_TEST(packet, flags_roundtrip) {
  Packet p;
  p.flags = kcd2s::wire::kFlagAckValid;
  auto buf = write_packet(p);
  Packet got;
  KCD2S_CHECK(read_packet(buf, got));
  KCD2S_CHECK_EQ(got.flags, kcd2s::wire::kFlagAckValid);
}

KCD2S_TEST(packet, reject_too_many_channels) {
  // Header is 2+2+2+4+1 = 11 bytes (incl. flags); channelCount is byte index 11. Claim 255
  // channels (> the cap) with no channel data behind it.
  std::vector<std::uint8_t> buf(12, 0);
  buf[11] = 255;
  Packet got;
  KCD2S_CHECK(!read_packet(buf, got));
}

KCD2S_TEST(packet, empty_buffer_rejected) {
  Packet got;
  KCD2S_CHECK(!read_packet(nullptr, 0, got));
}
