#include "kcd2s/transport/disconnect.hpp"

#include <cstdint>
#include <optional>
#include <vector>

#include "kcd2s/testkit.hpp"
#include "kcd2s/transport/handshake.hpp"
#include "kcd2s/wire/packet.hpp"

using kcd2s::transport::ClientHandshake;
using kcd2s::transport::ConnectionGate;
using kcd2s::transport::decode_disconnect_notice;
using kcd2s::transport::DisconnectNotice;
using kcd2s::transport::DisconnectReason;
using kcd2s::transport::encode_disconnect_notice;
using kcd2s::transport::is_disconnect_notice_frame;
using kcd2s::transport::notice_matches_session;
using kcd2s::transport::session_credentials_from_handshake;
using kcd2s::transport::SessionCredentials;

namespace {

using Frame = std::vector<std::uint8_t>;

SessionCredentials sample_session() {
  SessionCredentials session;
  session.server_nonce = 0x0102030405060708ull;
  session.cookie = 0x1112131415161718ull;
  return session;
}

// Runs the cookie handshake for one address and returns the CHALLENGE and CHALLENGE_RESPONSE.
struct AdmittedSession {
  Frame challenge;
  Frame response;
};

AdmittedSession admit(ConnectionGate& gate, std::uint64_t address, std::uint64_t client_nonce) {
  ClientHandshake client(client_nonce);
  AdmittedSession admitted;
  Frame accept;
  gate.on_datagram(address, client.start(), admitted.challenge);
  client.on_datagram(admitted.challenge, admitted.response);
  gate.on_datagram(address, admitted.response, accept);
  return admitted;
}

}  // namespace

KCD2S_TEST(disconnect, wire_contract_exact_bytes) {
  const Frame frame = encode_disconnect_notice(sample_session(), DisconnectReason::ClientQuit);
  const Frame expected = {
      0x06,                                            // kind
      0x53, 0x4B,                                      // protocolId 0x4B53, little-endian
      0x01,                                            // version
      0x01,                                            // reason: ClientQuit
      0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,  // serverNonce
      0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,  // cookie
  };
  KCD2S_CHECK_EQ(frame.size(), kcd2s::transport::kDisconnectNoticeSize);
  KCD2S_CHECK(frame == expected);
  KCD2S_CHECK_EQ(int{kcd2s::transport::kDisconnectNotice}, 6);
}

// 1-4 are the handshake frames, 5 is reserved for CONNECT_REFUSED.
static_assert(kcd2s::transport::kDisconnectNotice != kcd2s::transport::kConnectRequest);
static_assert(kcd2s::transport::kDisconnectNotice != kcd2s::transport::kChallenge);
static_assert(kcd2s::transport::kDisconnectNotice != kcd2s::transport::kChallengeResponse);
static_assert(kcd2s::transport::kDisconnectNotice != kcd2s::transport::kConnectAccept);
static_assert(kcd2s::transport::kDisconnectNotice != 5);

KCD2S_TEST(disconnect, decode_round_trip) {
  const Frame frame = encode_disconnect_notice(sample_session(), DisconnectReason::ClientQuit);
  const std::optional<DisconnectNotice> notice = decode_disconnect_notice(frame);
  KCD2S_CHECK(notice.has_value());
  if (!notice) {
    return;
  }
  KCD2S_CHECK_EQ(int{notice->version}, 1);
  KCD2S_CHECK_EQ(int{notice->reason}, 1);
  const SessionCredentials expected = sample_session();
  KCD2S_CHECK_EQ(notice->session.server_nonce, expected.server_nonce);
  KCD2S_CHECK_EQ(notice->session.cookie, expected.cookie);
  KCD2S_CHECK(notice_matches_session(*notice, expected));
}

KCD2S_TEST(disconnect, decode_rejects_malformed_frames) {
  const Frame valid = encode_disconnect_notice(sample_session(), DisconnectReason::ClientQuit);

  Frame truncated = valid;
  truncated.pop_back();
  KCD2S_CHECK(!decode_disconnect_notice(truncated).has_value());

  Frame version_one_with_trailer = valid;
  version_one_with_trailer.push_back(0);
  KCD2S_CHECK(!decode_disconnect_notice(version_one_with_trailer).has_value());

  Frame wrong_kind = valid;
  wrong_kind[0] = 5;
  KCD2S_CHECK(!decode_disconnect_notice(wrong_kind).has_value());

  Frame wrong_protocol = valid;
  wrong_protocol[1] = 0x51;  // protocol version 1
  KCD2S_CHECK(!decode_disconnect_notice(wrong_protocol).has_value());

  Frame version_zero = valid;
  version_zero[3] = 0;
  KCD2S_CHECK(!decode_disconnect_notice(version_zero).has_value());

  KCD2S_CHECK(!decode_disconnect_notice(Frame{}).has_value());
  KCD2S_CHECK(!decode_disconnect_notice(Frame(kcd2s::transport::kDisconnectNoticeSize, 0)));
}

KCD2S_TEST(disconnect, later_versions_are_additive_and_bounded) {
  Frame later = encode_disconnect_notice(sample_session(), DisconnectReason::ClientQuit);
  later[3] = 2;           // a future version
  later[4] = 0x7F;        // a reason this build does not know
  later.push_back(0xAA);  // a field appended by that version
  later.push_back(0xBB);
  const std::optional<DisconnectNotice> notice = decode_disconnect_notice(later);
  KCD2S_CHECK(notice.has_value());
  if (notice) {
    KCD2S_CHECK_EQ(int{notice->version}, 2);
    KCD2S_CHECK_EQ(int{notice->reason}, 0x7F);
    KCD2S_CHECK(notice_matches_session(*notice, sample_session()));
  }

  Frame oversized = later;
  oversized.resize(kcd2s::transport::kDisconnectNoticeMaxSize + 1, 0);
  KCD2S_CHECK(!is_disconnect_notice_frame(oversized));
  KCD2S_CHECK(!decode_disconnect_notice(oversized).has_value());

  Frame largest = later;
  largest.resize(kcd2s::transport::kDisconnectNoticeMaxSize, 0);
  KCD2S_CHECK(decode_disconnect_notice(largest).has_value());
}

KCD2S_TEST(disconnect, older_peers_drop_the_notice_as_channel_traffic) {
  // A server that predates the notice parses it as a KSTP packet. Either the parse fails or the
  // protocolId mismatches, and Connection::receive drops both.
  const Frame frame = encode_disconnect_notice(sample_session(), DisconnectReason::ClientQuit);
  kcd2s::wire::Packet packet;
  const bool parsed = kcd2s::wire::read_packet(frame, packet);
  KCD2S_CHECK(!parsed || packet.protocolId != kcd2s::wire::kProtocolId);
}

KCD2S_TEST(disconnect, handshake_frames_are_not_notices) {
  ConnectionGate gate(0xD15C0u);
  const AdmittedSession admitted = admit(gate, 0x1234u, 0xC11E47u);
  ClientHandshake fresh(0x77u);
  KCD2S_CHECK(!is_disconnect_notice_frame(fresh.start()));
  KCD2S_CHECK(!is_disconnect_notice_frame(admitted.challenge));
  KCD2S_CHECK(!is_disconnect_notice_frame(admitted.response));
}

KCD2S_TEST(disconnect, credentials_come_from_the_admitting_handshake) {
  const std::uint64_t secret = 0x5EC2E7ABCDull;
  const std::uint64_t address = 0xA0A0u;
  ConnectionGate gate(secret);
  const AdmittedSession admitted = admit(gate, address, 0xC0FFEEu);
  KCD2S_CHECK(gate.admitted(address));

  const std::optional<SessionCredentials> from_challenge =
      session_credentials_from_handshake(admitted.challenge);
  const std::optional<SessionCredentials> from_response =
      session_credentials_from_handshake(admitted.response);
  KCD2S_CHECK(from_challenge.has_value());
  KCD2S_CHECK(from_response.has_value());
  if (!from_challenge || !from_response) {
    return;
  }
  KCD2S_CHECK_EQ(from_challenge->server_nonce, from_response->server_nonce);
  KCD2S_CHECK_EQ(from_challenge->cookie, from_response->cookie);

  // The pair is the gate's own cookie for this address, so only the session holder knows it.
  const kcd2s::transport::CookieAuth auth(secret);
  KCD2S_CHECK_EQ(auth.cookie(address, from_response->server_nonce), from_response->cookie);

  ClientHandshake fresh(0x99u);
  KCD2S_CHECK(!session_credentials_from_handshake(fresh.start()).has_value());
  KCD2S_CHECK(!session_credentials_from_handshake(Frame{}).has_value());
}

KCD2S_TEST(disconnect, spoofed_or_foreign_session_is_rejected) {
  ConnectionGate gate(0xFEEDF00Du);
  const AdmittedSession victim = admit(gate, 0x1111u, 0xAAAAu);
  const AdmittedSession attacker = admit(gate, 0x2222u, 0xBBBBu);
  const std::optional<SessionCredentials> victim_session =
      session_credentials_from_handshake(victim.response);
  const std::optional<SessionCredentials> attacker_session =
      session_credentials_from_handshake(attacker.response);
  KCD2S_CHECK(victim_session.has_value());
  KCD2S_CHECK(attacker_session.has_value());
  if (!victim_session || !attacker_session) {
    return;
  }

  // The attacker's own valid session does not match the victim's stored pair.
  DisconnectNotice notice;
  notice.session = *attacker_session;
  KCD2S_CHECK(!notice_matches_session(notice, *victim_session));

  // Guessing either half is not enough.
  notice.session = *victim_session;
  notice.session.cookie ^= 1;
  KCD2S_CHECK(!notice_matches_session(notice, *victim_session));
  notice.session = *victim_session;
  notice.session.server_nonce ^= 1;
  KCD2S_CHECK(!notice_matches_session(notice, *victim_session));

  // A later session from the same address (new client nonce) gets a new pair, so a notice from
  // the previous session cannot remove it.
  gate.forget(0x1111u);
  const AdmittedSession rejoined = admit(gate, 0x1111u, 0xCCCCu);
  const std::optional<SessionCredentials> rejoined_session =
      session_credentials_from_handshake(rejoined.response);
  KCD2S_CHECK(rejoined_session.has_value());
  if (rejoined_session) {
    notice.session = *victim_session;
    KCD2S_CHECK(!notice_matches_session(notice, *rejoined_session));
  }

  notice.session = *victim_session;
  KCD2S_CHECK(notice_matches_session(notice, *victim_session));
}
