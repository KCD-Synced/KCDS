// KSTP connection handshake — stateless cookie challenge (platform §6).
//
// Before a peer is allowed to send channel traffic it must complete a three-way cookie handshake,
// so the server commits no per-connection state (and does no work beyond a hash) for a source that
// has not proven it can receive at its claimed address. This blunts spoofed-source floods: an
// attacker that cannot see the CHALLENGE cannot produce a valid CHALLENGE_RESPONSE.
//
//   client --CONNECT_REQUEST(protocolId, clientNonce)-->            server
//   client <--CHALLENGE(serverNonce, cookie)-------------            (stateless: no state kept)
//   client --CHALLENGE_RESPONSE(serverNonce, cookie, proof)-->      server verifies, admits addr
//   client <--CONNECT_ACCEPT-----------------------------            server
//
// Over capacity, a valid CONNECT_REQUEST or CHALLENGE_RESPONSE is answered with CONNECT_REFUSED
// instead (see encode_connect_refused). The refusal is stateless, never larger than the datagram
// it answers, and the caller rate-limits it globally (RefusalBudget), so it cannot be used for
// amplification. It echoes the client nonce (or the server nonce of a verified response), so an
// off-path sender cannot make a client give up a handshake it never attempted.
//
// A refusal of a verified CHALLENGE_RESPONSE may be preceded by REFUSAL_TEXT, the reason a
// playerConnecting handler gave (connection.reject(reason)). It is sent only to an address that
// proved it receives at its claimed address (the cookie verified), so its larger size cannot be
// used for amplification. Peers that predate it ignore the unknown kind and still receive the
// coded CONNECT_REFUSED; old servers never send it. No protocol id change is needed either way.
//
// cookie = keyedhash(secret, addr, serverNonce). `addr` is the TRANSPORT source address (the gate's
// caller supplies it from the datagram's origin), never trusted from the payload, so a response
// replayed from a different address recomputes a different cookie and fails. The server keeps no
// state for unverified peers; it only records an address once a valid response arrives.
//
// proof = the first 16 bytes of HMAC-SHA256(build key, "kcds-hs1" | serverNonce | cookie), both
// little-endian. Only a client built with the server's build key (core/version.hpp)
// can make it, and each server nonce is fresh, so a captured response proves nothing later. The
// server checks it in constant time; a response without a valid proof is dropped like a bad cookie.
//
// NOTE: the keyed hash here is a fast non-cryptographic mix — enough to gate spoofed sources that
// never saw the cookie. A production secret path would use SipHash/HMAC + a rotating secret; that
// is a drop-in replacement for CookieAuth and does not change the protocol. This is a transport
// slice that sits ABOVE the datagram link and BELOW Connection: once an address is admitted, the
// existing Connection carries its channel traffic unchanged.
#ifndef KCD2S_TRANSPORT_HANDSHAKE_HPP
#define KCD2S_TRANSPORT_HANDSHAKE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace kcd2s::transport {

// Fixed-width representation used in the Master's signed join pass. No floating
// point conversion: JavaScript cannot represent all 64-bit handshake nonces.
inline std::string licence_binding(std::uint64_t nonce) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result(16, '0');
  for (unsigned i = 0; i < 16; ++i) {
    result[15 - i] = digits[nonce & 15];
    nonce >>= 4;
  }
  return result;
}

enum HandshakeKind : std::uint8_t {
  kConnectRequest = 1,
  kChallenge = 2,
  kChallengeResponse = 3,
  kConnectAccept = 4,
  kConnectRefused = 5,  // added in; peers that predate it ignore the unknown kind
  kRefusalText = 6,     // added in; peers that predate it ignore the unknown kind
  kWaitStatus = 7,      // added in; peers that predate it ignore the unknown kind
  kJoinIdentity = 8,    // added in; peers that predate it ignore the unknown kind
};

// Why a handshake was refused. Clients treat every reason alike (back off and retry later).
enum class RefusalReason : std::uint8_t {
  ServerFull = 1,
  // A playerConnecting handler of the server's resources refused it.
  Rejected = 2,
  // The playerConnecting handlers did not decide in time (older clients treat it
  // like any other refusal).
  AdmissionTimeout = 3,
  // The server asks for a password and the join brought none or a wrong one; the
  // refusal text says which. The client asks the player for it and joins again.
  PasswordRequired = 4,
  // The client is another KCDS build than the server. It refuses a CONNECT_REQUEST
  // of this protocol whose build fingerprint is not the server's; the REFUSAL_TEXT that follows
  // names the server's version (other_version_text). Clients that predate it show the text with
  // a generic refusal.
  OtherVersion = 5,
  // The server's own ban list names the player (Player.ban, Bans, KCDS-Admin's Ban); the refusal
  // text says why. Clients that predate it show the text with a generic refusal.
  Banned = 6,
  // The server lets in only players whose licence pass carries a verified Discord account
  // (requireDiscord). The client offers to verify it and joins again. Clients
  // that predate it show the text with a generic refusal.
  DiscordRequired = 7,
  // The server has the Kingdom Come: Deliverance map (kcd1Map) and lets in only players whose
  // licence pass vouches that they own that game on Steam. The client asks the Master for such a
  // pass and joins again. Clients that predate it show the text with a generic refusal.
  Kcd1Required = 8,
};

// CONNECT_REFUSED wire layout, 14 bytes, LSB-first (byte-aligned fields are little-endian):
//   u8 kind (5) | u16 protocolId | u8 reason | u16 retryAfterMs | u64 echo
// `echo` is the client nonce of the refused CONNECT_REQUEST, or the server nonce of the refused
// CHALLENGE_RESPONSE. The frame carries the protocol id so it is versioned like CONNECT_REQUEST.
inline constexpr std::size_t kConnectRefusedSize = 14;
inline constexpr std::size_t kConnectRequestSize = 15;
// CHALLENGE_RESPONSE: u8 kind (3) | u64 serverNonce | u64 cookie | 16 bytes proof.
inline constexpr std::size_t kChallengeProofSize = 16;
inline constexpr std::size_t kChallengeResponseSize = 17 + kChallengeProofSize;

// REFUSAL_TEXT wire layout, LSB-first (byte-aligned fields are little-endian):
//   u8 kind (6) | u16 protocolId | u64 echo | u16 length | length bytes of UTF-8 text
// `echo` is the server nonce of the refused CHALLENGE_RESPONSE, as in the CONNECT_REFUSED that
// follows it. The text is plain text: at most kMaxRefusalTextBytes bytes of well-formed UTF-8
// without control or bidirectional formatting characters (see valid_refusal_text).
inline constexpr std::size_t kRefusalTextHeaderSize = 13;
// 512, as a playerConnecting handler's reject(reason) may give.
inline constexpr std::size_t kMaxRefusalTextBytes = 512;

struct RefusalText {
  std::uint64_t echo = 0;
  std::string text;
};

// True for a non-empty text of at most kMaxRefusalTextBytes bytes of well-formed UTF-8 with no
// C0/C1 control characters, DEL, or bidirectional embedding/override/isolate characters.
bool valid_refusal_text(std::string_view text);
// Makes any string a valid refusal text, or empty: malformed UTF-8 and the characters
// valid_refusal_text forbids are dropped (tab and line breaks become spaces), surrounding spaces
// are trimmed, and the result is cut to kMaxRefusalTextBytes on a character boundary.
std::string sanitize_refusal_text(std::string_view text);

struct ConnectRefusal {
  RefusalReason reason = RefusalReason::ServerFull;
  std::uint16_t retry_after_ms = 0;  // server's hint for the next attempt (0 = no hint)
  std::uint64_t echo = 0;
};

// Returns the leading kind byte, or 0 for an empty datagram (never a valid kind).
std::uint8_t handshake_kind(const std::vector<std::uint8_t>& dg);

// CONNECT_REQUEST carries the build fingerprint (core::build_fingerprint()) so a client of another
// build is turned away early. The 2-arg overload lets a caller/test supply an explicit fingerprint
// (e.g. to simulate a foreign client).
std::vector<std::uint8_t> encode_connect_request(std::uint64_t client_nonce);
std::vector<std::uint8_t> encode_connect_request(std::uint64_t client_nonce,
                                                 std::uint32_t build_fingerprint);
std::vector<std::uint8_t> encode_challenge(std::uint64_t server_nonce, std::uint64_t cookie);
// The response with this build's proof; the 3-arg overload proves under another key (a test's
// foreign client).
std::vector<std::uint8_t> encode_challenge_response(std::uint64_t server_nonce,
                                                    std::uint64_t cookie);
std::vector<std::uint8_t> encode_challenge_response(std::uint64_t server_nonce,
                                                    std::uint64_t cookie,
                                                    const std::array<std::uint8_t, 32>& key);
// The proof a CHALLENGE_RESPONSE carries under `key`.
std::array<std::uint8_t, kChallengeProofSize> challenge_proof(
    const std::array<std::uint8_t, 32>& key, std::uint64_t server_nonce, std::uint64_t cookie);
std::vector<std::uint8_t> encode_connect_accept();
std::vector<std::uint8_t> encode_connect_refused(const ConnectRefusal& refusal);
// Strict decode: exact size, kind and protocol id. Returns nullopt for anything else.
std::optional<ConnectRefusal> decode_connect_refused(const std::vector<std::uint8_t>& dg);
// `text` must satisfy valid_refusal_text (it is sanitized otherwise).
std::vector<std::uint8_t> encode_refusal_text(std::uint64_t echo, std::string_view text);
// Strict decode: kind, protocol id, exact size for the stated length, and a valid text.
std::optional<RefusalText> decode_refusal_text(const std::vector<std::uint8_t>& dg);

// The REFUSAL_TEXT of an OtherVersion refusal: it names the server's KCDS version, so a client of
// any version can show it.
std::string other_version_text(std::string_view server_version);
// The server version an other_version_text names ("0.3.4.1"), or empty for any other text.
std::string refused_version(std::string_view text);

// WAIT_STATUS, server -> client while the CHALLENGE_RESPONSE waits (a full server's
// queue, or the resources' playerConnecting), LSB-first:
//   u8 kind (7) | u16 protocolId | u64 echo | u16 place | u32 waitedSeconds | u16 line length |
//   line | u8 field count | per field: u8 label length, label, u8 value length, value
// `echo` is the server nonce of the waiting response; `place` is the place in the queue (1 is
// next), 0 when not queued. Every text is plain text as valid_refusal_text says (labels must not
// be empty; the line and values may be).
inline constexpr std::size_t kMaxWaitFields = 3;
inline constexpr std::size_t kMaxWaitLineBytes = 256;
inline constexpr std::size_t kMaxWaitLabelBytes = 32;
inline constexpr std::size_t kMaxWaitValueBytes = 96;

struct WaitField {
  std::string label;
  std::string value;

  friend bool operator==(const WaitField& a, const WaitField& b) {
    return a.label == b.label && a.value == b.value;
  }
};

struct WaitStatus {
  std::uint64_t echo = 0;
  std::uint16_t place = 0;
  std::uint32_t waited_seconds = 0;
  std::string line;
  std::vector<WaitField> fields;

  friend bool operator==(const WaitStatus& a, const WaitStatus& b) {
    return a.echo == b.echo && a.place == b.place && a.waited_seconds == b.waited_seconds &&
           a.line == b.line && a.fields == b.fields;
  }
};

// JOIN_IDENTITY, client -> server with every CHALLENGE_RESPONSE, LSB-first:
//   u8 kind (8) | u16 protocolId | u64 echo | u8 nickname length | nickname | u8 steamId length |
//   steamId [| u16 licence length | licence [| u8 discordId length | discordId |
//   u8 deviceId length | deviceId [| u8 passwordProof length | passwordProof [| u8 joinToken
//   length | joinToken]]]]
// `echo` is the server nonce the response answers. The nickname and Steam id are what the client
// reports, unverified: the nickname as valid_refusal_text says (or empty), at most
// kMaxIdentityNicknameBytes; the Steam id decimal digits, at most kMaxIdentitySteamIdBytes (or
// empty). The licence is the Master's signed join pass (the licence gate):
// base64url and '.', at most kMaxIdentityLicenceBytes, left out when empty (and when nothing
// follows it); the server's resources check it. The Discord user id (decimal digits, at most
// kMaxIdentityDiscordIdBytes) and the hardware id (kIdentityHardwareIdBytes lowercase hex, a
// hash of the machine's ids) are reported by the client, unverified; each may be empty, both are
// left out when both are and no password proof follows. The password proof is
// SHA-256 in lowercase hex of "<licence binding>:<password>" (the server nonce's binding, so a
// proof is good for this connection only and the password itself never travels); left out
// without a password, or empty when a join token follows. The join token is
// the text the player's launcher gave this join (kcds-launcher --join-token): 1 to
// kMaxIdentityJoinTokenBytes of letters, digits and - _ . ~ + / = :, unverified, left out
// without one.
inline constexpr std::size_t kMaxIdentityNicknameBytes = 64;
inline constexpr std::size_t kMaxIdentitySteamIdBytes = 20;
inline constexpr std::size_t kMaxIdentityLicenceBytes = 1024;
inline constexpr std::size_t kMaxIdentityDiscordIdBytes = 20;
inline constexpr std::size_t kIdentityHardwareIdBytes = 64;
inline constexpr std::size_t kIdentityPasswordProofBytes = 64;
inline constexpr std::size_t kMaxIdentityJoinTokenBytes = 255;

// Whether a text is a join token as JOIN_IDENTITY carries it (the launcher's valid_join_token).
bool join_token_text(std::string_view text);

struct JoinIdentity {
  std::uint64_t echo = 0;
  std::string nickname;
  std::string steam_id;
  std::string licence;
  std::string discord_id;
  std::string hardware_id;
  std::string password_proof;
  std::string join_token;

  friend bool operator==(const JoinIdentity& a, const JoinIdentity& b) {
    return a.echo == b.echo && a.nickname == b.nickname && a.steam_id == b.steam_id &&
           a.licence == b.licence && a.discord_id == b.discord_id &&
           a.hardware_id == b.hardware_id && a.password_proof == b.password_proof &&
           a.join_token == b.join_token;
  }
};

// The status cleaned to what travels (texts sanitized and cut, at most kMaxWaitFields fields,
// fields without a label dropped).
std::vector<std::uint8_t> encode_wait_status(const WaitStatus& status);
std::optional<WaitStatus> decode_wait_status(const std::vector<std::uint8_t>& dg);
// The identity cleaned to what travels (a nickname sanitized and cut, a Steam id that is not
// digits dropped, a licence that is not a pass dropped).
std::vector<std::uint8_t> encode_join_identity(const JoinIdentity& identity);
std::optional<JoinIdentity> decode_join_identity(const std::vector<std::uint8_t>& dg);

// Stateless keyed-cookie generator/verifier. Holds only the server secret.
class CookieAuth {
 public:
  explicit CookieAuth(std::uint64_t secret) : secret_(secret) {
  }
  // A per-request server nonce derived deterministically (binding the cookie to the secret is what
  // provides the security, not nonce secrecy).
  std::uint64_t server_nonce(std::uint64_t addr, std::uint64_t client_nonce) const;
  // The cookie bound to this address + server nonce under the secret.
  std::uint64_t cookie(std::uint64_t addr, std::uint64_t server_nonce) const;

  // A resume token bound to a logical session id under the secret (NOT the address, so a client may
  // resume from a changed address). Any (session, token) pair that verifies is one the server
  // issued; a bearer token, so capture = hijack (documented) — a production build would bind more.
  std::uint64_t session_token(std::uint64_t session_id) const;

 private:
  std::uint64_t secret_;
};

// Server-side admission gate. Stateless for unverified peers; records only admitted addresses.
class ConnectionGate {
 public:
  explicit ConnectionGate(std::uint64_t secret) : auth_(secret) {
  }

  // Process one inbound datagram that arrived from transport address `addr`. On a CONNECT_REQUEST,
  // fills out_reply with a CHALLENGE; on a valid CHALLENGE_RESPONSE, admits `addr` and fills
  // out_reply with a CONNECT_ACCEPT. Returns true iff out_reply should be sent back to `addr`.
  // Any non-handshake, malformed, or invalid-cookie datagram returns false and admits nothing.
  bool on_datagram(std::uint64_t addr, const std::vector<std::uint8_t>& dg,
                   std::vector<std::uint8_t>& out_reply);

  // Over-capacity path. Validates `dg` exactly as on_datagram would (protocol and build gates for
  // a CONNECT_REQUEST, cookie for a CHALLENGE_RESPONSE) but admits nothing and keeps no state; a
  // valid handshake fills out_reply with CONNECT_REFUSED. Returns true iff out_reply should be
  // sent. The caller is responsible for the global rate limit (RefusalBudget).
  bool refuse(std::uint64_t addr, const std::vector<std::uint8_t>& dg, RefusalReason reason,
              std::uint16_t retry_after_ms, std::vector<std::uint8_t>& out_reply) const;
  // The REFUSAL_TEXT to send before refuse()'s CONNECT_REFUSED. Only a CHALLENGE_RESPONSE whose
  // cookie verifies earns one, and only for a text that is non-empty once sanitized. Returns true
  // iff out_reply should be sent.
  bool refusal_text(std::uint64_t addr, const std::vector<std::uint8_t>& dg, std::string_view text,
                    std::vector<std::uint8_t>& out_reply) const;
  // A CONNECT_REQUEST of this protocol from a client of another build: the
  // CONNECT_REFUSED (OtherVersion, echoing the client nonce) and then the REFUSAL_TEXT naming
  // `server_version`, to be sent in this order. False for anything else. The request is
  // unauthenticated and the answer larger, so the caller limits how often it answers.
  bool refuse_other_build(const std::vector<std::uint8_t>& dg, std::string_view server_version,
                          std::vector<std::uint8_t>& out_refusal,
                          std::vector<std::uint8_t>& out_text) const;
  // A CHALLENGE_RESPONSE from `addr` whose cookie verifies: its server nonce (what a WAIT_STATUS
  // and the JOIN_IDENTITY of that response echo). Nullopt for anything else; admits nothing.
  std::optional<std::uint64_t> verified_response(std::uint64_t addr,
                                                 const std::vector<std::uint8_t>& dg) const;

  bool admitted(std::uint64_t addr) const {
    return admitted_.count(addr) != 0;
  }
  std::size_t admitted_count() const {
    return admitted_.size();
  }
  void forget(std::uint64_t addr) {
    admitted_.erase(addr);
  }

  // Resume-token mint/verify under the gate's secret (used by ServerEndpoint reconnect).
  std::uint64_t session_token(std::uint64_t session_id) const {
    return auth_.session_token(session_id);
  }

 private:
  CookieAuth auth_;
  std::unordered_set<std::uint64_t> admitted_;
};

// Client-side handshake state machine.
class ClientHandshake {
 public:
  enum class State { Idle, AwaitingChallenge, AwaitingAccept, Established, Refused };

  explicit ClientHandshake(std::uint64_t client_nonce) : nonce_(client_nonce) {
  }

  // Produce the initial CONNECT_REQUEST and advance to AwaitingChallenge. Also starts a new
  // attempt after a refusal.
  std::vector<std::uint8_t> start();

  // Feed a datagram from the server. On CHALLENGE (while awaiting one) fills out_reply with the
  // CHALLENGE_RESPONSE and returns true; on CONNECT_ACCEPT marks Established and returns false.
  // A CONNECT_REFUSED whose echo matches this attempt moves to Refused and returns false (see
  // refusal()). A REFUSAL_TEXT for this attempt's response is kept for refusal_text(), whether it
  // arrives before or after its CONNECT_REFUSED. Anything else is ignored (returns false).
  bool on_datagram(const std::vector<std::uint8_t>& dg, std::vector<std::uint8_t>& out_reply);

  State state() const {
    return state_;
  }
  bool established() const {
    return state_ == State::Established;
  }
  bool refused() const {
    return state_ == State::Refused;
  }
  // The last accepted refusal (meaningful while refused()).
  const ConnectRefusal& refusal() const {
    return refusal_;
  }
  // The server's reason for the last accepted refusal, or empty when it gave none.
  std::string refusal_text() const;
  // The last WAIT_STATUS for this attempt's response, while it waits.
  const std::optional<WaitStatus>& wait_status() const {
    return wait_;
  }
  // The server nonce of this attempt's CHALLENGE, once it came (a JOIN_IDENTITY echoes it).
  std::uint64_t server_nonce() const {
    return server_nonce_;
  }

 private:
  std::uint64_t nonce_;
  std::uint64_t server_nonce_ = 0;  // from the CHALLENGE; a refused response echoes it
  State state_ = State::Idle;
  ConnectRefusal refusal_;
  bool has_refusal_ = false;
  RefusalText text_;  // the last REFUSAL_TEXT accepted; it belongs to the refusal with its echo
  std::optional<WaitStatus> wait_;  // the last WAIT_STATUS of this attempt's response
};

// Global token bucket for CONNECT_REFUSED replies: at most `burst` at once and `per_second` on
// average across all sources. Deliberately not per address, so the server keeps no state for
// refused sources; a flood can only exhaust the refusal budget, after which over-capacity
// handshakes go unanswered exactly as they did before refusals existed.
class RefusalBudget {
 public:
  RefusalBudget(double per_second, double burst)
      : per_second_(per_second), burst_(burst), tokens_(burst) {
  }

  // Takes one token if one is available at time `now` (seconds, monotonic).
  bool try_take(double now);

 private:
  double per_second_;
  double burst_;
  double tokens_;
  double last_ = -1.0;
};

// Client-side pacing of handshake datagrams.
// - An unanswered datagram is re-sent after 0.25 s; the interval doubles after every four
//   unanswered sends, up to 2 s, so an unreachable or silent server is not hammered.
// - A refusal waits for the server's retry-after hint (at least 1 s), doubled for each
//   consecutive refusal, at most 60 s, plus up to 25% jitter derived from the seed so refused
//   clients do not retry in lockstep.
// - Handshake progress (a CHALLENGE) resets the unanswered backoff; an accepted handshake ends
//   pacing, and a later fresh attempt starts from the base interval again.
class HandshakePacer {
 public:
  static constexpr double kResendInterval = 0.25;
  static constexpr double kMaxResendInterval = 2.0;
  static constexpr unsigned kSendsPerResendStep = 4;
  static constexpr double kMinRefusalDelay = 1.0;
  static constexpr double kMaxRefusalDelay = 60.0;

  explicit HandshakePacer(std::uint64_t jitter_seed);

  // True when the next handshake datagram should be sent at `now`.
  bool due(double now) const {
    return now >= next_send_;
  }
  double next_send() const {
    return next_send_;
  }
  // A handshake datagram was sent at `now`; schedules the resend if nothing answers it.
  void sent(double now);
  // The server answered and the handshake advanced.
  void progressed();
  // The server refused at `now`; schedules the next attempt and returns its delay in seconds.
  double refused(double now, std::uint16_t retry_after_ms);
  unsigned consecutive_refusals() const {
    return consecutive_refusals_;
  }

 private:
  double jitter_fraction_;  // in [0, 0.25)
  double next_send_ = 0.0;
  unsigned unanswered_ = 0;
  unsigned consecutive_refusals_ = 0;
};

}  // namespace kcd2s::transport

#endif  // KCD2S_TRANSPORT_HANDSHAKE_HPP
