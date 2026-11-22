#include "discord_presence.hpp"
#include "../loader_link/payload_link.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

#include "kcd2s/integration/script_channel.hpp"
#include "kcds/loader/kcds_abi.h"

namespace kcds::runtime::discord {
namespace {

namespace sdk = kcd2s::sdk;

// The loader's interface with Discord in it (version 7).
constexpr std::uint32_t kLoaderDiscord = 7;
// How often a waiting authorization looks for Discord's answer.
constexpr DWORD kAnswerPollMs = 50;

struct Status {
  bool available = false;
  std::string user_id;

  friend bool operator==(const Status& a, const Status& b) {
    return a.available == b.available && a.user_id == b.user_id;
  }
};

std::mutex state_mutex;
Presence presence;
std::string version_text;
Logger log_line = nullptr;
bool started = false;
// The runtime that was last told the status, and what it was told.
const void* told_session = nullptr;
std::optional<Status> told_status;

std::int64_t unix_now() {
  using clock = std::chrono::system_clock;
  return std::chrono::duration_cast<std::chrono::seconds>(clock::now().time_since_epoch()).count();
}

std::string app_id() {
  char text[64] = {};
  const DWORD length = GetEnvironmentVariableA("KCDS_DISCORD_APP_ID", text, sizeof(text));
  if (length > 0 && length < sizeof(text)) {
    return text;
  }
  return kDefaultAppId;
}

const KcdsLoaderApi* loader() {
  return kcds::runtime::loader_link::loader_api(kLoaderDiscord);
}

// Hands the activity to the loader, which publishes it (state_mutex held).
void publish_locked() {
  if (!started) {
    return;
  }
  if (const KcdsLoaderApi* api = loader()) {
    api->discord_set_activity(activity_json(presence, version_text).c_str());
  }
}

Status current_status() {
  Status now;
  if (const KcdsLoaderApi* api = loader()) {
    char user[32] = {};
    now.available = api->discord_status(user, sizeof(user)) == 1;
    now.user_id = user;
  }
  return now;
}

std::string text(const sdk::Value& message, const char* name) {
  const sdk::Value* value = kcd2s::integration::script_field(message, name);
  return value && value->tag() == sdk::Tag::Str ? value->as_str() : std::string{};
}

}  // namespace

void start(const std::string& version, Logger log) {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (started) {
    return;
  }
  started = true;
  version_text = version;
  log_line = log;
  presence.since = unix_now();
  const KcdsLoaderApi* api = loader();
  if (!api) {
    if (log_line) {
      log_line(
          "DISCORD the KCDS loader has no Discord link (older than interface 7): presence off");
    }
    return;
  }
  // The loader holds the connection; a client loaded by a swap finds it connected.
  api->discord_start(app_id().c_str());
  publish_locked();
}

void set_scene(Scene scene, const std::string& server) {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (presence.scene == scene && presence.server == server) {
    return;
  }
  const bool same_server = (scene == Scene::Playing || scene == Scene::Joining) &&
                           (presence.scene == Scene::Playing || presence.scene == Scene::Joining) &&
                           presence.server == server;
  if (!same_server) {
    presence.script = ScriptPresence{};
    presence.players = 0;
    presence.max_players = 0;
  }
  presence.scene = scene;
  presence.server = server;
  presence.since = unix_now();
  publish_locked();
}

void entered_world() {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (presence.scene == Scene::Playing) {
    return;
  }
  if (presence.scene != Scene::Joining) {
    presence.server.clear();
  }
  presence.scene = Scene::Playing;
  presence.since = unix_now();
  publish_locked();
}

std::string user_id() {
  const Status now = current_status();
  return now.available ? now.user_id : std::string();
}

std::string authorize(unsigned timeout_ms) {
  const KcdsLoaderApi* api = loader();
  if (!api || api->discord_authorize(1) != 1) {
    return {};
  }
  // The loader carries the round trip; this thread only looks for the answer, and gives up when
  // the client is about to be unloaded.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline && !kcds::runtime::loader_link::stopping()) {
    char code[300] = {};
    const int state = api->discord_authorization(code, sizeof(code));
    if (state == 1) {
      return code;
    }
    if (state < 0) {
      return {};
    }
    Sleep(kAnswerPollMs);
  }
  // The player did not answer in time: a late answer is dropped.
  api->discord_authorize(0);
  return {};
}

bool handle_runtime_message(const sdk::Value& message) {
  const std::string operation = kcd2s::integration::script_operation(message);
  if (operation == "discordPresence") {
    std::lock_guard<std::mutex> lock(state_mutex);
    presence.script.name = text(message, "name");
    presence.script.state = text(message, "state");
    presence.script.large_image = text(message, "largeImage");
    presence.script.large_text = text(message, "largeText");
    presence.script.small_image = text(message, "smallImage");
    presence.script.small_text = text(message, "smallText");
    presence.script.button_label = text(message, "buttonLabel");
    presence.script.button_url = text(message, "buttonUrl");
    publish_locked();
    return true;
  }
  // The server's player count and places (its reserved client event kcds:playerCount).
  if (operation == "discordPlayers") {
    const sdk::Value* players = kcd2s::integration::script_field(message, "players");
    const sdk::Value* places = kcd2s::integration::script_field(message, "max");
    std::lock_guard<std::mutex> lock(state_mutex);
    const auto whole = [](const sdk::Value* value) {
      if (value && value->tag() == sdk::Tag::Int) {
        return int(value->as_int());
      }
      if (value && value->tag() == sdk::Tag::Double && value->as_double() >= 0 &&
          value->as_double() < 100000) {
        return int(value->as_double());
      }
      return 0;
    };
    presence.players = whole(players);
    presence.max_players = whole(places);
    publish_locked();
    return true;
  }
  if (operation == "discordClear") {
    std::lock_guard<std::mutex> lock(state_mutex);
    presence.script = ScriptPresence{};
    publish_locked();
    return true;
  }
  return false;
}

void pump(const void* session, const RuntimeSender& send) {
  const Status now = current_status();
  if (session == told_session && told_status && *told_status == now) {
    return;
  }
  sdk::Value::Map message{{"op", sdk::Value(std::string("discordStatus"))},
                          {"available", sdk::Value(now.available)},
                          {"userId", sdk::Value(now.user_id)}};
  if (send(sdk::Value(std::move(message)))) {
    told_session = session;
    told_status = now;
  }
}

}  // namespace kcds::runtime::discord
