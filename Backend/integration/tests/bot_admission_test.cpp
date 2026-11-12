// Bots (kcds-bots) on a server: only a development build that asked for them lets one in, and only
// a client with the build's handshake key gets that far.
#include "kcd2s/integration/bot_admission.hpp"

#include <array>
#include <cstdint>
#include <vector>

#include "kcd2s/core/bits.hpp"
#include "kcd2s/core/version.hpp"
#include "kcd2s/testkit.hpp"
#include "kcd2s/transport/handshake.hpp"

using kcd2s::integration::bot_admission;
using kcd2s::integration::BotAdmission;
using kcd2s::integration::kBotLicence;
using kcd2s::transport::JoinIdentity;

namespace {

JoinIdentity bot() {
  JoinIdentity identity;
  identity.nickname = "bot-1";
  identity.licence = std::string(kBotLicence);
  return identity;
}

}  // namespace

KCD2S_TEST(bot_admission, a_player_is_never_a_bot) {
  JoinIdentity player;
  player.licence = "eyJ2IjoxfQ.c2lnbmF0dXJl";
  KCD2S_CHECK(bot_admission(player, true, true) == BotAdmission::NotBot);
  KCD2S_CHECK(bot_admission(JoinIdentity{}, true, true) == BotAdmission::NotBot);
  // Only the exact text: a pass that starts like it is a pass.
  player.licence = std::string(kBotLicence) + "x";
  KCD2S_CHECK(bot_admission(player, true, true) == BotAdmission::NotBot);
}

KCD2S_TEST(bot_admission, a_bot_needs_kcds_allow_bots) {
  KCD2S_CHECK(bot_admission(bot(), false, true) == BotAdmission::RefusedNotAllowed);
  KCD2S_CHECK(bot_admission(bot(), true, true) == BotAdmission::Allowed);
  KCD2S_CHECK(
      !std::string(kcd2s::integration::bot_refusal_log(BotAdmission::RefusedNotAllowed)).empty());
}

KCD2S_TEST(bot_admission, a_release_key_build_never_admits_a_bot) {
  KCD2S_CHECK(bot_admission(bot(), true, false) == BotAdmission::RefusedReleaseKey);
  KCD2S_CHECK(bot_admission(bot(), false, false) == BotAdmission::RefusedReleaseKey);
}

KCD2S_TEST(bot_admission, the_bot_licence_travels_in_the_identity) {
  JoinIdentity identity = bot();
  identity.echo = 0x1234u;
  const auto bytes = kcd2s::transport::encode_join_identity(identity);
  const auto decoded = kcd2s::transport::decode_join_identity(bytes);
  KCD2S_CHECK(decoded.has_value());
  KCD2S_CHECK(decoded->licence == kBotLicence);
}

// A bot proves the handshake with its build's key like any client: one built with another key (a
// release's) never gets past the gate, so the server never even sees its identity.
KCD2S_TEST(bot_admission, only_the_builds_key_passes_the_handshake) {
  kcd2s::transport::ConnectionGate gate(0x5A17u);
  const std::uint64_t address = 0xB07u;
  std::vector<std::uint8_t> challenge;
  KCD2S_CHECK(
      gate.on_datagram(address, kcd2s::transport::encode_connect_request(0x42u), challenge));
  kcd2s::core::BitReader reader(challenge);
  reader.read_bits(8);
  const std::uint64_t server_nonce = reader.read_bits(64);
  const std::uint64_t cookie = reader.read_bits(64);
  std::array<std::uint8_t, 32> other_key = kcd2s::core::handshake_key();
  other_key[5] ^= 0x10;
  std::vector<std::uint8_t> reply;
  KCD2S_CHECK(!gate.on_datagram(
      address, kcd2s::transport::encode_challenge_response(server_nonce, cookie, other_key),
      reply));
  KCD2S_CHECK(!gate.admitted(address));
  KCD2S_CHECK(gate.on_datagram(
      address, kcd2s::transport::encode_challenge_response(server_nonce, cookie), reply));
  KCD2S_CHECK(gate.admitted(address));
}

// The development key is what an unconfigured build has; the test build is one.
KCD2S_TEST(bot_admission, a_build_without_a_secret_has_the_development_key) {
  KCD2S_CHECK(kcd2s::core::kDevelopmentHandshakeKey);
}
