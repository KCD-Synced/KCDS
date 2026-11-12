#pragma once
// Bots: the load-testing clients of kcds-bots (Backend/integration/local/bots). A bot joins with
// the licence text kBotLicence in its JOIN_IDENTITY instead of a Master join pass. A server lets
// one in only when its owner asked for bots (KCDS_ALLOW_BOTS=1) and the server was built with the
// development handshake key: a published server has the release key, so a bot cannot even finish
// the handshake there, and a published build never admits one whatever its settings say. An
// admitted bot skips only the licence gate (it owns no game); bans, the password and the
// resources' playerConnecting handlers apply as for every player. Bots are labelled in the log and
// in the server's metrics.
#include <string_view>

#include "kcd2s/core/version.hpp"
#include "kcd2s/transport/handshake.hpp"

namespace kcd2s::integration {

// The licence text a bot sends: base64url characters, so it travels as a pass would.
inline constexpr std::string_view kBotLicence = "kcds-bot";

// What a bot is told when the server turns it away.
inline constexpr std::string_view kBotRefusalText = "This server does not accept bots.";

enum class BotAdmission {
  NotBot,             // a player: the licence gate decides
  Allowed,            // a bot the server lets in
  RefusedNotAllowed,  // a bot, and KCDS_ALLOW_BOTS is not set
  RefusedReleaseKey,  // a bot, and this build has the release handshake key
};

inline bool bot_identity(const transport::JoinIdentity& identity) {
  return identity.licence == kBotLicence;
}

// Whether the join of `identity` is a bot and may come in. `allow_bots`: KCDS_ALLOW_BOTS;
// `development_key`: the build's handshake key is the development one (core/version.hpp).
inline BotAdmission bot_admission(const transport::JoinIdentity& identity, bool allow_bots,
                                  bool development_key) {
  if (!bot_identity(identity)) {
    return BotAdmission::NotBot;
  }
  if (!development_key) {
    return BotAdmission::RefusedReleaseKey;
  }
  if (!allow_bots) {
    return BotAdmission::RefusedNotAllowed;
  }
  return BotAdmission::Allowed;
}

// The server log's line for a refused bot.
inline const char* bot_refusal_log(BotAdmission admission) {
  switch (admission) {
    case BotAdmission::RefusedReleaseKey:
      return "JOIN refused: a bot (kcds-bots) cannot join a build with the release handshake key";
    case BotAdmission::RefusedNotAllowed:
      return "JOIN refused: a bot (kcds-bots); bots join only when the server runs with "
             "KCDS_ALLOW_BOTS=1";
    default:
      return "";
  }
}

}  // namespace kcd2s::integration
