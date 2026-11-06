#include "kcd2s/transport/disconnect.hpp"

#include "kcd2s/core/bits.hpp"
#include "kcd2s/transport/handshake.hpp"
#include "kcd2s/wire/packet.hpp"

namespace kcd2s::transport {

using core::BitReader;
using core::BitWriter;

namespace {

// CHALLENGE and CHALLENGE_RESPONSE start alike: u8 kind | u64 serverNonce | u64 cookie. The
// response carries the build key's proof after them.
constexpr std::size_t kChallengeFrameSize = 17;

}  // namespace

std::optional<SessionCredentials> session_credentials_from_handshake(
    const std::vector<std::uint8_t>& frame) {
  const std::uint8_t kind = handshake_kind(frame);
  const bool challenge = kind == kChallenge && frame.size() == kChallengeFrameSize;
  const bool response = kind == kChallengeResponse && frame.size() == kChallengeResponseSize;
  if (!challenge && !response) {
    return std::nullopt;
  }
  BitReader reader(frame);
  reader.read_bits(8);  // kind
  SessionCredentials session;
  session.server_nonce = reader.read_bits(64);
  session.cookie = reader.read_bits(64);
  if (reader.overrun()) {
    return std::nullopt;
  }
  return session;
}

std::vector<std::uint8_t> encode_disconnect_notice(const SessionCredentials& session,
                                                   DisconnectReason reason) {
  BitWriter writer;
  writer.write_bits(kDisconnectNotice, 8);
  writer.write_bits(wire::kProtocolId, 16);
  writer.write_bits(kDisconnectNoticeVersion, 8);
  writer.write_bits(static_cast<std::uint8_t>(reason), 8);
  writer.write_bits(session.server_nonce, 64);
  writer.write_bits(session.cookie, 64);
  return writer.bytes();
}

bool is_disconnect_notice_frame(const std::vector<std::uint8_t>& frame) {
  if (frame.size() < kDisconnectNoticeSize || frame.size() > kDisconnectNoticeMaxSize) {
    return false;
  }
  return frame[0] == kDisconnectNotice;
}

std::optional<DisconnectNotice> decode_disconnect_notice(const std::vector<std::uint8_t>& frame) {
  if (!is_disconnect_notice_frame(frame)) {
    return std::nullopt;
  }
  BitReader reader(frame);
  reader.read_bits(8);  // kind
  const auto protocol_id = static_cast<std::uint16_t>(reader.read_bits(16));
  DisconnectNotice notice;
  notice.version = static_cast<std::uint8_t>(reader.read_bits(8));
  notice.reason = static_cast<std::uint8_t>(reader.read_bits(8));
  notice.session.server_nonce = reader.read_bits(64);
  notice.session.cookie = reader.read_bits(64);
  if (reader.overrun() || protocol_id != wire::kProtocolId) {
    return std::nullopt;
  }
  if (notice.version == 0) {
    return std::nullopt;
  }
  // Version 1 has a fixed size; only later versions may append fields.
  if (notice.version == kDisconnectNoticeVersion && frame.size() != kDisconnectNoticeSize) {
    return std::nullopt;
  }
  return notice;
}

bool notice_matches_session(const DisconnectNotice& notice, const SessionCredentials& session) {
  const std::uint64_t nonce_difference = notice.session.server_nonce ^ session.server_nonce;
  const std::uint64_t cookie_difference = notice.session.cookie ^ session.cookie;
  return (nonce_difference | cookie_difference) == 0;
}

}  // namespace kcd2s::transport
