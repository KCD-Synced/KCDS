// KSTP graceful disconnect notice (client -> server)..
//
// A client that leaves on purpose tells the server immediately instead of letting the server find
// out through its inactivity timeout. The notice is a standalone frame next to the handshake
// frames (handshake.hpp), not a channel message, so it needs no Connection state and works even
// while the reliable channels are backed up.
//
// DISCONNECT_NOTICE wire layout, version 1, 21 bytes, little-endian:
//
//   offset  0  u8   kind          6 (kDisconnectNotice)
//   offset  1  u16  protocolId    wire::kProtocolId (0x4B53, bytes 53 4B)
//   offset  3  u8   version       1 (kDisconnectNoticeVersion)
//   offset  4  u8   reason        DisconnectReason; unknown values are accepted
//   offset  5  u64  serverNonce   the session's CHALLENGE server nonce
//   offset 13  u64  cookie        the session's CHALLENGE cookie
//
// Frame kind numbering: 1-4 are the handshake frames (handshake.hpp), 5 is reserved for
// CONNECT_REFUSED, 6 is this notice. A KSTP packet starts with the protocolId low
// byte 0x52, so the leading kind byte never collides with channel traffic.
//
// Authentication: the (serverNonce, cookie) pair is the one the server's ConnectionGate verified
// when it admitted this transport address. The server stores that pair per member and accepts a
// notice only if it arrives from the member's transport address AND carries exactly the stored
// pair. An off-path sender never saw the CHALLENGE, so it cannot remove another player; a notice
// replayed against a later session of the same address carries the old pair and is ignored. As
// with the rest of KSTP (no per-packet authentication yet), an on-path
// observer of the handshake is out of scope.
//
// Versioning is additive. Decoders accept any version >= 1 and ignore bytes appended by later
// versions, up to kDisconnectNoticeMaxSize. Peers that predate the notice ignore it: the old
// server routes it as a packet whose protocolId (0x5306) mismatches and Connection drops it, and
// the inactivity timeout still removes the client.
//
// Delivery: UDP may drop the notice, so the sender emits kDisconnectNoticeCopies copies back to
// back. Handling is idempotent (the first valid copy removes the member, later copies find no
// member and are ignored). The inactivity timeout remains the fallback when every copy is lost.
#ifndef KCD2S_TRANSPORT_DISCONNECT_HPP
#define KCD2S_TRANSPORT_DISCONNECT_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace kcd2s::transport {

inline constexpr std::uint8_t kDisconnectNotice = 6;
inline constexpr std::uint8_t kDisconnectNoticeVersion = 1;
inline constexpr std::size_t kDisconnectNoticeSize = 21;
inline constexpr std::size_t kDisconnectNoticeMaxSize = 64;
inline constexpr unsigned kDisconnectNoticeCopies = 3;

enum class DisconnectReason : std::uint8_t {
  ClientQuit = 1,  // the player closed the game or left the server on purpose
};

// The handshake values that identify one admitted session: the server nonce and cookie of the
// CHALLENGE the client answered (CHALLENGE and CHALLENGE_RESPONSE carry the same pair).
struct SessionCredentials {
  std::uint64_t server_nonce = 0;
  std::uint64_t cookie = 0;
};

struct DisconnectNotice {
  std::uint8_t version = kDisconnectNoticeVersion;
  std::uint8_t reason = static_cast<std::uint8_t>(DisconnectReason::ClientQuit);
  SessionCredentials session;
};

// Extracts the session pair from a 17-byte CHALLENGE or CHALLENGE_RESPONSE frame. Returns nullopt
// for any other frame.
std::optional<SessionCredentials> session_credentials_from_handshake(
    const std::vector<std::uint8_t>& frame);

std::vector<std::uint8_t> encode_disconnect_notice(const SessionCredentials& session,
                                                   DisconnectReason reason);

// Cheap routing check: leading kind byte and a size inside the accepted bounds. A frame that
// passes still has to decode and match the session.
bool is_disconnect_notice_frame(const std::vector<std::uint8_t>& frame);

// Strict decode: kind, protocolId, version >= 1, size between kDisconnectNoticeSize and
// kDisconnectNoticeMaxSize (exactly kDisconnectNoticeSize for version 1).
std::optional<DisconnectNotice> decode_disconnect_notice(const std::vector<std::uint8_t>& frame);

// True when the notice carries exactly the pair the server stored when it admitted the session.
// The comparison does not branch on which half differs.
bool notice_matches_session(const DisconnectNotice& notice, const SessionCredentials& session);

}  // namespace kcd2s::transport

#endif  // KCD2S_TRANSPORT_DISCONNECT_HPP
