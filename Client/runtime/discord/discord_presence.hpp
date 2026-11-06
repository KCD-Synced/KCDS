// The player's Discord activity: KCDS's own presence through the
// Discord desktop app's local IPC (the named pipe discord-ipc-0 to 9), as discord_activity.hpp
// composes it.
//
// It needs the KCDS Discord application's id: the environment variable KCDS_DISCORD_APP_ID, else
// kDefaultAppId. Without the Discord app, nothing is published and the client
// resources' Discord.isAvailable() is false. A worker thread connects (again every 15 seconds
// while it cannot), publishes the activity when it changed (at most every 4 seconds; Discord
// allows 5 updates in 20 seconds) and answers Discord's pings.
//
// The client resources (Client/resources/discord.cjs) send { op: 'discordPresence', name, state,
// largeImage, largeText, smallImage, smallText, buttonLabel, buttonUrl } and
// { op: 'discordClear' }; the game tells them { op:
// 'discordStatus', available, userId } when that changes and to every new resource runtime.
#pragma once

#include <functional>
#include <string>

#include "discord_activity.hpp"
#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::discord {

using Logger = void (*)(const char* format, ...);
using RuntimeSender = std::function<bool(const kcd2s::sdk::Value&)>;

// The KCDS Discord application ("KCDS"; KCDS_DISCORD_APP_ID overrides it).
inline constexpr char kDefaultAppId[] = "1563839669496578088";

// Starts the worker; the activity shows "Starting up" until the first scene.
void start(const std::string& version, Logger log);

// What the player does now; `server` is the server's name while joining or playing. A new scene
// restarts the timer; leaving a server clears what the resources added.
void set_scene(Scene scene, const std::string& server = {});

// The player's game is in the server's world: playing, on the server being joined (a launcher's
// direct join has no browser scene before it).
void entered_world();

// The Discord user signed in to the player's Discord app, "" until Discord answered (the
// server's player.discordId, reported by the client). Any thread.
std::string user_id();

// Asks the player's Discord app to let the KCDS application know who they are (4.11 request
// 39): Discord shows them its window, once per account, and answers with a code that the KCDS
// Master trades for their Discord user. Waits up to timeout_ms; "" when Discord is not running,
// the player declined or did not answer in time. Any thread but the worker.
std::string authorize(unsigned timeout_ms);

// A resource runtime message; false when it is not a Discord one. Any thread.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Tells the current resource runtime the Discord status when it changed or the runtime is new
// (network_mutex held).
void pump(const void* session, const RuntimeSender& send);

}  // namespace kcds::runtime::discord
