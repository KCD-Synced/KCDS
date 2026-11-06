#include "kcd2s/transport/handshake.hpp"

#include "kcd2s/core/bits.hpp"
#include "kcd2s/core/crypto/sha256.hpp"
#include "kcd2s/core/version.hpp"
#include "kcd2s/wire/packet.hpp"

namespace kcd2s::transport {

using core::BitReader;
using core::BitWriter;

namespace {

// splitmix64 finalizer — a fast avalanche mix (NOT cryptographic; see header note).
std::uint64_t mix(std::uint64_t input) {
  constexpr std::uint64_t kIncrement = 0x9E3779B97F4A7C15ull;
  constexpr std::uint64_t kFirstFactor = 0xBF58476D1CE4E5B9ull;
  constexpr std::uint64_t kSecondFactor = 0x94D049BB133111EBull;
  const std::uint64_t offset = input + kIncrement;
  const std::uint64_t first = (offset ^ (offset >> 30)) * kFirstFactor;
  const std::uint64_t second = (first ^ (first >> 27)) * kSecondFactor;
  return second ^ (second >> 31);
}

std::uint64_t keyed_hash(std::uint64_t secret, std::uint64_t a, std::uint64_t b) {
  return mix(secret ^ mix(a ^ mix(b ^ 0xD1B54A32D192ED03ull)));
}

// Parses a CONNECT_REQUEST and applies the protocol and build-fingerprint gates. A mismatched
// version or a client of another build is rejected before any cookie work.
bool accept_connect_request(const std::vector<std::uint8_t>& dg, std::uint64_t& client_nonce) {
  BitReader r(dg);
  r.read_bits(8);  // kind
  const std::uint16_t proto = static_cast<std::uint16_t>(r.read_bits(16));
  const std::uint32_t build_fp = static_cast<std::uint32_t>(r.read_bits(32));
  client_nonce = r.read_bits(64);
  return !r.overrun() && proto == wire::kProtocolId && build_fp == core::build_fingerprint();
}

// A CONNECT_REQUEST of this protocol whose build fingerprint is another build's.
bool other_build_request(const std::vector<std::uint8_t>& dg, std::uint64_t& client_nonce) {
  if (dg.size() != kConnectRequestSize || handshake_kind(dg) != kConnectRequest) {
    return false;
  }
  BitReader r(dg);
  r.read_bits(8);  // kind
  const std::uint16_t proto = static_cast<std::uint16_t>(r.read_bits(16));
  const std::uint32_t build_fp = static_cast<std::uint32_t>(r.read_bits(32));
  client_nonce = r.read_bits(64);
  return !r.overrun() && proto == wire::kProtocolId && build_fp != core::build_fingerprint();
}

constexpr std::string_view kOtherVersionLead = "This server runs KCDS ";

// A release version: digits and dots, starting and ending with a digit, at most 32 characters.
bool release_version(std::string_view text) {
  if (text.empty() || text.size() > 32 || text.front() == '.' || text.back() == '.') {
    return false;
  }
  for (const char c : text) {
    if (c != '.' && (c < '0' || c > '9')) {
      return false;
    }
  }
  return true;
}

// Parses a CHALLENGE_RESPONSE and verifies its cookie against the TRANSPORT address + echoed
// nonce (a cookie minted for another address or under another secret cannot match), then its
// build proof: only a client built with this build's key makes the right one.
bool verify_challenge_response(const CookieAuth& auth, std::uint64_t addr,
                               const std::vector<std::uint8_t>& dg, std::uint64_t& server_nonce) {
  if (dg.size() != kChallengeResponseSize)
    return false;
  BitReader r(dg);
  r.read_bits(8);  // kind
  server_nonce = r.read_bits(64);
  const std::uint64_t cookie = r.read_bits(64);
  if (r.overrun())
    return false;
  if (auth.cookie(addr, server_nonce) != cookie)
    return false;
  const auto proof = challenge_proof(core::handshake_key(), server_nonce, cookie);
  const std::uint8_t* sent = dg.data() + (kChallengeResponseSize - kChallengeProofSize);
  return core::crypto::equal_in_constant_time(proof.data(), sent, kChallengeProofSize);
}

// Decodes the UTF-8 sequence at text[at]. Returns its length (1-4) and the code point, or 0 for a
// malformed, overlong or surrogate sequence.
std::size_t utf8_sequence(std::string_view text, std::size_t at, char32_t& code_point) {
  const auto lead = static_cast<unsigned char>(text[at]);
  std::size_t length = 0;
  char32_t minimum = 0;
  if (lead < 0x80) {
    code_point = lead;
    return 1;
  } else if ((lead & 0xE0) == 0xC0) {
    length = 2;
    minimum = 0x80;
    code_point = lead & 0x1F;
  } else if ((lead & 0xF0) == 0xE0) {
    length = 3;
    minimum = 0x800;
    code_point = lead & 0x0F;
  } else if ((lead & 0xF8) == 0xF0) {
    length = 4;
    minimum = 0x10000;
    code_point = lead & 0x07;
  } else {
    return 0;
  }
  if (at + length > text.size()) {
    return 0;
  }
  for (std::size_t i = 1; i < length; ++i) {
    const auto next = static_cast<unsigned char>(text[at + i]);
    if ((next & 0xC0) != 0x80) {
      return 0;
    }
    code_point = (code_point << 6) | (next & 0x3F);
  }
  const bool surrogate = code_point >= 0xD800 && code_point <= 0xDFFF;
  if (code_point < minimum || code_point > 0x10FFFF || surrogate) {
    return 0;
  }
  return length;
}

// Control characters and the bidirectional formatting characters that could reorder the text
// around it. A refusal text is shown as written, so none of them may reach the player.
bool forbidden_in_refusal(char32_t c) {
  const bool control = c < 0x20 || (c >= 0x7F && c <= 0x9F);
  const bool bidi = (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069);
  return control || bidi;
}

}  // namespace

bool valid_refusal_text(std::string_view text) {
  if (text.empty() || text.size() > kMaxRefusalTextBytes) {
    return false;
  }
  for (std::size_t at = 0; at < text.size();) {
    char32_t c = 0;
    const std::size_t length = utf8_sequence(text, at, c);
    if (length == 0 || forbidden_in_refusal(c)) {
      return false;
    }
    at += length;
  }
  return true;
}

std::string sanitize_refusal_text(std::string_view text) {
  std::string clean;
  for (std::size_t at = 0; at < text.size();) {
    char32_t c = 0;
    const std::size_t length = utf8_sequence(text, at, c);
    if (length == 0) {
      at += 1;  // a malformed byte is dropped
      continue;
    }
    const bool tab_or_line_break = c == 0x09 || c == 0x0A || c == 0x0D;
    if (tab_or_line_break) {
      clean.push_back(' ');
    } else if (!forbidden_in_refusal(c)) {
      clean.append(text.substr(at, length));
    }
    at += length;
  }
  const std::size_t first = clean.find_first_not_of(' ');
  if (first == std::string::npos) {
    return {};
  }
  clean = clean.substr(first, clean.find_last_not_of(' ') - first + 1);
  if (clean.size() > kMaxRefusalTextBytes) {
    std::size_t cut = kMaxRefusalTextBytes;
    while (cut > 0 && (static_cast<unsigned char>(clean[cut]) & 0xC0) == 0x80) {
      cut -= 1;  // back to the first byte of the character that does not fit
    }
    clean.resize(cut);
    const std::size_t last = clean.find_last_not_of(' ');
    clean.resize(last == std::string::npos ? 0 : last + 1);
  }
  return clean;
}

std::uint8_t handshake_kind(const std::vector<std::uint8_t>& dg) {
  return dg.empty() ? std::uint8_t{0} : dg[0];
}

std::vector<std::uint8_t> encode_connect_request(std::uint64_t client_nonce,
                                                 std::uint32_t build_fingerprint) {
  BitWriter w;
  w.write_bits(kConnectRequest, 8);
  w.write_bits(wire::kProtocolId, 16);  // early protocol gate
  w.write_bits(build_fingerprint, 32);  // build-salt compatibility gate
  w.write_bits(client_nonce, 64);
  return w.bytes();
}

std::vector<std::uint8_t> encode_connect_request(std::uint64_t client_nonce) {
  return encode_connect_request(client_nonce, core::build_fingerprint());
}

std::vector<std::uint8_t> encode_challenge(std::uint64_t server_nonce, std::uint64_t cookie) {
  BitWriter w;
  w.write_bits(kChallenge, 8);
  w.write_bits(server_nonce, 64);
  w.write_bits(cookie, 64);
  return w.bytes();
}

std::array<std::uint8_t, kChallengeProofSize> challenge_proof(
    const std::array<std::uint8_t, 32>& key, std::uint64_t server_nonce, std::uint64_t cookie) {
  std::uint8_t message[24] = {'k', 'c', 'd', 's', '-', 'h', 's', '1'};
  for (int i = 0; i < 8; ++i) {
    message[8 + i] = static_cast<std::uint8_t>(server_nonce >> (8 * i));
    message[16 + i] = static_cast<std::uint8_t>(cookie >> (8 * i));
  }
  const auto mac = core::crypto::hmac_sha256(key.data(), key.size(), message, sizeof(message));
  std::array<std::uint8_t, kChallengeProofSize> proof{};
  for (std::size_t i = 0; i < kChallengeProofSize; ++i) {
    proof[i] = mac[i];
  }
  return proof;
}

std::vector<std::uint8_t> encode_challenge_response(std::uint64_t server_nonce,
                                                    std::uint64_t cookie,
                                                    const std::array<std::uint8_t, 32>& key) {
  BitWriter w;
  w.write_bits(kChallengeResponse, 8);
  w.write_bits(server_nonce, 64);
  w.write_bits(cookie, 64);
  std::vector<std::uint8_t> bytes = w.bytes();
  const auto proof = challenge_proof(key, server_nonce, cookie);
  bytes.insert(bytes.end(), proof.begin(), proof.end());
  return bytes;
}

std::vector<std::uint8_t> encode_challenge_response(std::uint64_t server_nonce,
                                                    std::uint64_t cookie) {
  return encode_challenge_response(server_nonce, cookie, core::handshake_key());
}

std::vector<std::uint8_t> encode_connect_accept() {
  BitWriter w;
  w.write_bits(kConnectAccept, 8);
  return w.bytes();
}

std::vector<std::uint8_t> encode_connect_refused(const ConnectRefusal& refusal) {
  BitWriter w;
  w.write_bits(kConnectRefused, 8);
  w.write_bits(wire::kProtocolId, 16);
  w.write_bits(static_cast<std::uint8_t>(refusal.reason), 8);
  w.write_bits(refusal.retry_after_ms, 16);
  w.write_bits(refusal.echo, 64);
  return w.bytes();
}

std::optional<ConnectRefusal> decode_connect_refused(const std::vector<std::uint8_t>& dg) {
  if (dg.size() != kConnectRefusedSize || handshake_kind(dg) != kConnectRefused)
    return std::nullopt;
  BitReader r(dg);
  r.read_bits(8);  // kind
  const std::uint16_t proto = static_cast<std::uint16_t>(r.read_bits(16));
  ConnectRefusal refusal;
  refusal.reason = static_cast<RefusalReason>(r.read_bits(8));
  refusal.retry_after_ms = static_cast<std::uint16_t>(r.read_bits(16));
  refusal.echo = r.read_bits(64);
  if (r.overrun() || proto != wire::kProtocolId)
    return std::nullopt;
  return refusal;
}

std::vector<std::uint8_t> encode_refusal_text(std::uint64_t echo, std::string_view text) {
  const std::string clean =
      valid_refusal_text(text) ? std::string(text) : sanitize_refusal_text(text);
  BitWriter w;
  w.write_bits(kRefusalText, 8);
  w.write_bits(wire::kProtocolId, 16);
  w.write_bits(echo, 64);
  w.write_bits(clean.size(), 16);
  for (const char c : clean) {
    w.write_bits(static_cast<unsigned char>(c), 8);
  }
  return w.bytes();
}

std::string other_version_text(std::string_view server_version) {
  std::string text(kOtherVersionLead);
  text += server_version;
  text += ". Your KCDS is another version: join with KCDS ";
  text += server_version;
  text += '.';
  return text;
}

std::string refused_version(std::string_view text) {
  if (text.substr(0, kOtherVersionLead.size()) != kOtherVersionLead) {
    return {};
  }
  const std::string_view rest = text.substr(kOtherVersionLead.size());
  const std::size_t end = rest.find(". ");
  if (end == std::string_view::npos || !release_version(rest.substr(0, end))) {
    return {};
  }
  return std::string(rest.substr(0, end));
}

std::optional<RefusalText> decode_refusal_text(const std::vector<std::uint8_t>& dg) {
  if (dg.size() < kRefusalTextHeaderSize || handshake_kind(dg) != kRefusalText)
    return std::nullopt;
  BitReader r(dg);
  r.read_bits(8);  // kind
  const std::uint16_t proto = static_cast<std::uint16_t>(r.read_bits(16));
  RefusalText refusal;
  refusal.echo = r.read_bits(64);
  const std::size_t length = static_cast<std::size_t>(r.read_bits(16));
  if (r.overrun() || proto != wire::kProtocolId || length > kMaxRefusalTextBytes ||
      dg.size() != kRefusalTextHeaderSize + length) {
    return std::nullopt;
  }
  refusal.text.assign(dg.begin() + kRefusalTextHeaderSize, dg.end());
  if (!valid_refusal_text(refusal.text))
    return std::nullopt;
  return refusal;
}

namespace {

// Writes a length-prefixed text (u8 or u16 length).
void write_text(BitWriter& w, const std::string& text, unsigned length_bits) {
  w.write_bits(text.size(), length_bits);
  for (const char c : text) {
    w.write_bits(static_cast<unsigned char>(c), 8);
  }
}

bool read_text(BitReader& r, std::string& text, unsigned length_bits, std::size_t max) {
  const std::size_t length = static_cast<std::size_t>(r.read_bits(length_bits));
  if (r.overrun() || length > max) {
    return false;
  }
  text.clear();
  for (std::size_t i = 0; i < length; ++i) {
    text.push_back(static_cast<char>(r.read_bits(8)));
  }
  return !r.overrun();
}

// A text cleaned for the wire, cut to `max` bytes on a character boundary.
std::string wire_text(std::string_view text, std::size_t max) {
  std::string clean = valid_refusal_text(text) ? std::string(text) : sanitize_refusal_text(text);
  if (clean.size() > max) {
    std::size_t cut = max;
    while (cut > 0 && (static_cast<unsigned char>(clean[cut]) & 0xC0) == 0x80) {
      --cut;
    }
    clean.resize(cut);
  }
  return clean;
}

bool text_ok(const std::string& text, std::size_t max, bool may_be_empty) {
  if (text.empty()) {
    return may_be_empty;
  }
  return text.size() <= max && valid_refusal_text(text);
}

bool digits(const std::string& text) {
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  return true;
}

// A licence pass: base64url text and its '.' separator, not empty, at most
// kMaxIdentityLicenceBytes.
bool pass_text(const std::string& text) {
  if (text.empty() || text.size() > kMaxIdentityLicenceBytes) {
    return false;
  }
  for (const char c : text) {
    const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    const bool digit = c >= '0' && c <= '9';
    if (!letter && !digit && c != '-' && c != '_' && c != '.') {
      return false;
    }
  }
  return true;
}

// A hardware id: kIdentityHardwareIdBytes lowercase hex characters.
bool hardware_text(const std::string& text) {
  return text.size() == kIdentityHardwareIdBytes &&
         text.find_first_not_of("0123456789abcdef") == std::string::npos;
}

}  // namespace

std::vector<std::uint8_t> encode_wait_status(const WaitStatus& status) {
  BitWriter w;
  w.write_bits(kWaitStatus, 8);
  w.write_bits(wire::kProtocolId, 16);
  w.write_bits(status.echo, 64);
  w.write_bits(status.place, 16);
  w.write_bits(status.waited_seconds, 32);
  write_text(w, wire_text(status.line, kMaxWaitLineBytes), 16);
  std::vector<WaitField> fields;
  for (const WaitField& field : status.fields) {
    WaitField clean{wire_text(field.label, kMaxWaitLabelBytes),
                    wire_text(field.value, kMaxWaitValueBytes)};
    if (!clean.label.empty() && fields.size() < kMaxWaitFields) {
      fields.push_back(std::move(clean));
    }
  }
  w.write_bits(fields.size(), 8);
  for (const WaitField& field : fields) {
    write_text(w, field.label, 8);
    write_text(w, field.value, 8);
  }
  return w.bytes();
}

std::optional<WaitStatus> decode_wait_status(const std::vector<std::uint8_t>& dg) {
  if (handshake_kind(dg) != kWaitStatus) {
    return std::nullopt;
  }
  BitReader r(dg);
  r.read_bits(8);
  const std::uint16_t proto = static_cast<std::uint16_t>(r.read_bits(16));
  WaitStatus status;
  status.echo = r.read_bits(64);
  status.place = static_cast<std::uint16_t>(r.read_bits(16));
  status.waited_seconds = static_cast<std::uint32_t>(r.read_bits(32));
  if (r.overrun() || proto != wire::kProtocolId ||
      !read_text(r, status.line, 16, kMaxWaitLineBytes) ||
      !text_ok(status.line, kMaxWaitLineBytes, true)) {
    return std::nullopt;
  }
  const std::size_t count = static_cast<std::size_t>(r.read_bits(8));
  if (r.overrun() || count > kMaxWaitFields) {
    return std::nullopt;
  }
  for (std::size_t i = 0; i < count; ++i) {
    WaitField field;
    if (!read_text(r, field.label, 8, kMaxWaitLabelBytes) ||
        !read_text(r, field.value, 8, kMaxWaitValueBytes) ||
        !text_ok(field.label, kMaxWaitLabelBytes, false) ||
        !text_ok(field.value, kMaxWaitValueBytes, true)) {
      return std::nullopt;
    }
    status.fields.push_back(std::move(field));
  }
  if (r.bits_remaining() != 0) {
    return std::nullopt;
  }
  return status;
}

bool join_token_text(std::string_view text) {
  if (text.empty() || text.size() > kMaxIdentityJoinTokenBytes) {
    return false;
  }
  for (const char c : text) {
    const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    const bool digit = c >= '0' && c <= '9';
    const bool mark = c == '-' || c == '_' || c == '.' || c == '~' || c == '+' || c == '/' ||
                      c == '=' || c == ':';
    if (!letter && !digit && !mark) {
      return false;
    }
  }
  return true;
}

std::vector<std::uint8_t> encode_join_identity(const JoinIdentity& identity) {
  BitWriter w;
  w.write_bits(kJoinIdentity, 8);
  w.write_bits(wire::kProtocolId, 16);
  w.write_bits(identity.echo, 64);
  write_text(w, wire_text(identity.nickname, kMaxIdentityNicknameBytes), 8);
  const bool steam =
      identity.steam_id.size() <= kMaxIdentitySteamIdBytes && digits(identity.steam_id);
  write_text(w, steam ? identity.steam_id : std::string(), 8);
  const bool licence = pass_text(identity.licence);
  const bool discord =
      identity.discord_id.size() <= kMaxIdentityDiscordIdBytes && digits(identity.discord_id);
  const bool hardware = hardware_text(identity.hardware_id);
  // A password proof has the hardware id's form: 64 lowercase hex characters.
  const bool password = hardware_text(identity.password_proof);
  const bool token = join_token_text(identity.join_token);
  const bool extras = (discord && !identity.discord_id.empty()) || hardware || password || token;
  if (licence || extras) {
    write_text(w, licence ? identity.licence : std::string(), 16);
  }
  if (extras) {
    write_text(w, discord ? identity.discord_id : std::string(), 8);
    write_text(w, hardware ? identity.hardware_id : std::string(), 8);
  }
  if (password || token) {
    write_text(w, password ? identity.password_proof : std::string(), 8);
  }
  if (token) {
    write_text(w, identity.join_token, 8);
  }
  return w.bytes();
}

std::optional<JoinIdentity> decode_join_identity(const std::vector<std::uint8_t>& dg) {
  if (handshake_kind(dg) != kJoinIdentity) {
    return std::nullopt;
  }
  BitReader r(dg);
  r.read_bits(8);
  const std::uint16_t proto = static_cast<std::uint16_t>(r.read_bits(16));
  JoinIdentity identity;
  identity.echo = r.read_bits(64);
  if (r.overrun() || proto != wire::kProtocolId ||
      !read_text(r, identity.nickname, 8, kMaxIdentityNicknameBytes) ||
      !text_ok(identity.nickname, kMaxIdentityNicknameBytes, true) ||
      !read_text(r, identity.steam_id, 8, kMaxIdentitySteamIdBytes) || !digits(identity.steam_id)) {
    return std::nullopt;
  }
  // The licence pass is there when the client has one or reports more after it.
  if (r.bits_remaining() != 0) {
    const bool read = read_text(r, identity.licence, 16, kMaxIdentityLicenceBytes);
    if (!read || (!identity.licence.empty() && !pass_text(identity.licence))) {
      return std::nullopt;
    }
  }
  if (r.bits_remaining() != 0) {
    const bool discord = read_text(r, identity.discord_id, 8, kMaxIdentityDiscordIdBytes) &&
                         digits(identity.discord_id);
    const bool hardware = read_text(r, identity.hardware_id, 8, kIdentityHardwareIdBytes) &&
                          (identity.hardware_id.empty() || hardware_text(identity.hardware_id));
    if (!discord || !hardware) {
      return std::nullopt;
    }
  }
  if (r.bits_remaining() != 0) {
    // Empty only when a join token follows.
    const bool read = read_text(r, identity.password_proof, 8, kIdentityPasswordProofBytes);
    const bool proof = identity.password_proof.empty() ? r.bits_remaining() != 0
                                                       : hardware_text(identity.password_proof);
    if (!read || !proof) {
      return std::nullopt;
    }
  }
  if (r.bits_remaining() != 0) {
    const bool read = read_text(r, identity.join_token, 8, kMaxIdentityJoinTokenBytes);
    if (!read || !join_token_text(identity.join_token)) {
      return std::nullopt;
    }
  }
  if (r.bits_remaining() != 0) {
    return std::nullopt;
  }
  return identity;
}

std::uint64_t CookieAuth::server_nonce(std::uint64_t addr, std::uint64_t client_nonce) const {
  return keyed_hash(secret_ ^ 0x5331u, addr, client_nonce);
}

std::uint64_t CookieAuth::cookie(std::uint64_t addr, std::uint64_t server_nonce) const {
  return keyed_hash(secret_, addr, server_nonce);
}

std::uint64_t CookieAuth::session_token(std::uint64_t session_id) const {
  return keyed_hash(secret_ ^ 0x5E5510A1u, session_id, 0xB10Bu);
}

bool ConnectionGate::on_datagram(std::uint64_t addr, const std::vector<std::uint8_t>& dg,
                                 std::vector<std::uint8_t>& out_reply) {
  switch (handshake_kind(dg)) {
    case kConnectRequest: {
      std::uint64_t cnonce = 0;
      if (!accept_connect_request(dg, cnonce))
        return false;
      const std::uint64_t sn = auth_.server_nonce(addr, cnonce);
      out_reply = encode_challenge(sn, auth_.cookie(addr, sn));  // no state kept
      return true;
    }
    case kChallengeResponse: {
      std::uint64_t sn = 0;
      if (!verify_challenge_response(auth_, addr, dg, sn))
        return false;
      admitted_.insert(addr);
      out_reply = encode_connect_accept();
      return true;
    }
    default:
      return false;  // non-handshake / unknown kind: ignore, admit nothing
  }
}

bool ConnectionGate::refuse(std::uint64_t addr, const std::vector<std::uint8_t>& dg,
                            RefusalReason reason, std::uint16_t retry_after_ms,
                            std::vector<std::uint8_t>& out_reply) const {
  ConnectRefusal refusal;
  refusal.reason = reason;
  refusal.retry_after_ms = retry_after_ms;
  switch (handshake_kind(dg)) {
    case kConnectRequest: {
      if (!accept_connect_request(dg, refusal.echo))
        return false;
      break;
    }
    case kChallengeResponse: {
      if (!verify_challenge_response(auth_, addr, dg, refusal.echo))
        return false;
      break;
    }
    default:
      return false;  // only a valid handshake earns a refusal
  }
  out_reply = encode_connect_refused(refusal);  // no state kept, nothing admitted
  return true;
}

bool ConnectionGate::refusal_text(std::uint64_t addr, const std::vector<std::uint8_t>& dg,
                                  std::string_view text,
                                  std::vector<std::uint8_t>& out_reply) const {
  std::uint64_t server_nonce = 0;
  if (handshake_kind(dg) != kChallengeResponse ||
      !verify_challenge_response(auth_, addr, dg, server_nonce)) {
    return false;  // only a verified address may receive the larger frame
  }
  const std::string clean = sanitize_refusal_text(text);
  if (clean.empty())
    return false;
  out_reply = encode_refusal_text(server_nonce, clean);
  return true;
}

bool ConnectionGate::refuse_other_build(const std::vector<std::uint8_t>& dg,
                                        std::string_view server_version,
                                        std::vector<std::uint8_t>& out_refusal,
                                        std::vector<std::uint8_t>& out_text) const {
  ConnectRefusal refusal;
  refusal.reason = RefusalReason::OtherVersion;
  if (!other_build_request(dg, refusal.echo) || !release_version(server_version)) {
    return false;
  }
  out_refusal = encode_connect_refused(refusal);
  out_text = encode_refusal_text(refusal.echo, other_version_text(server_version));
  return true;
}

std::optional<std::uint64_t> ConnectionGate::verified_response(
    std::uint64_t addr, const std::vector<std::uint8_t>& dg) const {
  std::uint64_t server_nonce = 0;
  if (handshake_kind(dg) != kChallengeResponse ||
      !verify_challenge_response(auth_, addr, dg, server_nonce)) {
    return std::nullopt;
  }
  return server_nonce;
}

bool RefusalBudget::try_take(double now) {
  if (last_ >= 0.0 && now > last_) {
    tokens_ += (now - last_) * per_second_;
    if (tokens_ > burst_)
      tokens_ = burst_;
  }
  if (last_ < 0.0 || now > last_)
    last_ = now;
  if (tokens_ < 1.0)
    return false;
  tokens_ -= 1.0;
  return true;
}

HandshakePacer::HandshakePacer(std::uint64_t jitter_seed)
    : jitter_fraction_(static_cast<double>(jitter_seed % 1000u) / 4000.0) {
}

void HandshakePacer::sent(double now) {
  double interval = kResendInterval;
  const unsigned steps = unanswered_ / kSendsPerResendStep;
  for (unsigned i = 0; i < steps && interval < kMaxResendInterval; ++i)
    interval *= 2.0;
  if (interval > kMaxResendInterval)
    interval = kMaxResendInterval;
  unanswered_ += 1;
  next_send_ = now + interval;
}

void HandshakePacer::progressed() {
  unanswered_ = 0;
}

double HandshakePacer::refused(double now, std::uint16_t retry_after_ms) {
  consecutive_refusals_ += 1;
  double delay = static_cast<double>(retry_after_ms) / 1000.0;
  if (delay < kMinRefusalDelay)
    delay = kMinRefusalDelay;
  for (unsigned i = 1; i < consecutive_refusals_ && delay < kMaxRefusalDelay; ++i)
    delay *= 2.0;
  if (delay > kMaxRefusalDelay)
    delay = kMaxRefusalDelay;
  delay += delay * jitter_fraction_;
  unanswered_ = 0;
  next_send_ = now + delay;
  return delay;
}

std::string ClientHandshake::refusal_text() const {
  if (!has_refusal_ || text_.text.empty() || text_.echo != refusal_.echo)
    return {};
  return text_.text;
}

std::vector<std::uint8_t> ClientHandshake::start() {
  state_ = State::AwaitingChallenge;
  wait_.reset();
  return encode_connect_request(nonce_);
}

bool ClientHandshake::on_datagram(const std::vector<std::uint8_t>& dg,
                                  std::vector<std::uint8_t>& out_reply) {
  switch (handshake_kind(dg)) {
    case kChallenge: {
      if (state_ != State::AwaitingChallenge)
        return false;
      BitReader r(dg);
      r.read_bits(8);
      const std::uint64_t sn = r.read_bits(64);
      const std::uint64_t ck = r.read_bits(64);
      if (r.overrun())
        return false;
      out_reply = encode_challenge_response(sn, ck);
      server_nonce_ = sn;
      state_ = State::AwaitingAccept;
      return true;
    }
    case kConnectAccept:
      if (state_ == State::AwaitingAccept)
        state_ = State::Established;
      return false;  // no reply
    case kConnectRefused: {
      const auto refusal = decode_connect_refused(dg);
      if (!refusal)
        return false;
      // Accept only a refusal of this attempt's own request or response.
      const bool refuses_request = state_ == State::AwaitingChallenge && refusal->echo == nonce_;
      const bool refuses_response =
          state_ == State::AwaitingAccept && refusal->echo == server_nonce_;
      if (!refuses_request && !refuses_response)
        return false;
      refusal_ = *refusal;
      has_refusal_ = true;
      state_ = State::Refused;
      return false;  // no reply; the caller paces the next attempt
    }
    case kWaitStatus: {
      auto status = decode_wait_status(dg);
      // A status of this attempt's waiting response only.
      if (status && state_ == State::AwaitingAccept && status->echo == server_nonce_) {
        wait_ = std::move(status);
      }
      return false;
    }
    case kRefusalText: {
      auto text = decode_refusal_text(dg);
      if (!text)
        return false;
      // The text of this attempt's refused (or about to be refused) response only.
      const bool before_refusal = state_ == State::AwaitingAccept && text->echo == server_nonce_;
      const bool after_refusal = state_ == State::Refused && text->echo == refusal_.echo;
      if (before_refusal || after_refusal)
        text_ = std::move(*text);
      return false;
    }
    default:
      return false;
  }
}

}  // namespace kcd2s::transport
