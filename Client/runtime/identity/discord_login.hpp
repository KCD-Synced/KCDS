// Verifying the player's Discord account, only when they choose to: a server
// that asks for a verified Discord account refused them and they pressed Verify Discord.
//
// When the Discord app runs, its own window asks them to allow the KCDS Discord application
// (scope identify, the IPC AUTHORIZE of discord_presence.hpp). Without the app, or when they close
// that window, the game opens Discord's sign-in page in their browser, and Discord sends them
// back to the game's own listener on kDiscordRedirect with a code. The game hands the code with
// the link ticket of its last licence proof to the KCDS Master (POST /v1/discord/link), which
// trades it with Discord and keeps the Discord account with the player's store account. Their
// next pass carries it, verified.
#pragma once

#include <optional>
#include <string>

#include "discord_callback.hpp"

namespace kcds::runtime::identity {

using Logger = void (*)(const char* format, ...);

// The listener's port, kDiscordRedirect's (discord_callback.hpp): among the KCDS application's
// redirects and the Master's KCDS_DISCORD_REDIRECT_URI.
inline constexpr unsigned short kDiscordLoginPort = 47811;
// How long the player has to finish signing in.
inline constexpr unsigned kDiscordLoginSeconds = 180;

// The link ticket of the last licence proof (the Master gives one while a pass holds no verified
// Discord account), kept for a few minutes. Any thread.
void remember_discord_ticket(const std::string& master_url, const std::string& ticket);

enum class DiscordLogin { Waiting, Verified, Failed };

struct DiscordLoginResult {
  DiscordLogin state = DiscordLogin::Failed;
  // For the player: what went wrong, "" when verified.
  std::string message;
};

// Starts the sign-in on its own thread; false when one is already running.
bool start_discord_login(Logger log);

// The sign-in's outcome, once, when it ended.
std::optional<DiscordLoginResult> take_discord_login_result();

}  // namespace kcds::runtime::identity
