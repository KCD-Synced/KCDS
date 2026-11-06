// What KCDS shows as the player's Discord activity, and the frames of
// Discord's local IPC that carry it. Pure; discord_presence.cpp talks to Discord.
//
// KCDS's own presence: the game starting, the main menu, joining a server and playing on it,
// with the KCDS logo (asset key kLogoAsset) and the client version as its large image, a timer
// since that state began, and the KCDS website button. While the player is on a server a client
// resource may name the server (the first line reads "KCDS - <name>"; Discord shows the app's own
// name, KCDS, as the title), set the status line (state), the logo (large image) and the small
// image with their tooltips, and one button of its own before KCDS's
// (Client/resources/discord.cjs). KCDS's website button is always the last one; without a
// resource's fields the presence is KCDS's own.
//
// Discord's IPC: frames of a little-endian op (0 handshake, 1 frame, 2 close, 3 ping, 4 pong) and
// length, then that many bytes of JSON.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace kcds::runtime::discord {

inline constexpr char kLogoAsset[] = "kcds";
// KCDS's own button, always the last one.
inline constexpr char kWebsiteLabel[] = "KCDS website";
inline constexpr char kWebsiteUrl[] = "https://kcdsynced.com";
// Discord's limits for a button's label and link (bytes).
inline constexpr std::size_t kMaxButtonLabelBytes = 32;
inline constexpr std::size_t kMaxButtonUrlBytes = 512;
// Discord's limit for a text field (bytes); shorter than two characters is refused.
inline constexpr std::size_t kMaxFieldBytes = 128;

enum class Scene { Starting, Menu, Joining, Playing };

struct ScriptPresence {
  std::string name;
  std::string state;
  std::string large_image;
  std::string large_text;
  std::string small_image;
  std::string small_text;
  std::string button_label;
  std::string button_url;

  friend bool operator==(const ScriptPresence& a, const ScriptPresence& b) {
    return a.name == b.name && a.state == b.state && a.large_image == b.large_image &&
           a.large_text == b.large_text && a.small_image == b.small_image &&
           a.small_text == b.small_text && a.button_label == b.button_label &&
           a.button_url == b.button_url;
  }
};

struct Presence {
  Scene scene = Scene::Starting;
  std::string server;      // the server's name while joining or playing
  std::int64_t since = 0;  // Unix seconds the scene began, 0 for none
  ScriptPresence script;
  // How many play on the server of how many places (the server's kcds:playerCount), 0 and 0 while
  // not known: shown as "(players of places)" after the status line while playing.
  int players = 0;
  int max_players = 0;
};

// The status line beside the player count when no resource set one.
inline constexpr char kPlayersState[] = "Players online";

// A JSON string with its quotes; control characters escaped.
inline std::string json_string(const std::string& text) {
  std::string out = "\"";
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (byte < 0x20) {
      char escaped[8];
      std::snprintf(escaped, sizeof(escaped), "\\u%04x", unsigned(byte));
      out += escaped;
    } else {
      out += c;
    }
  }
  return out + "\"";
}

// The text cut to Discord's limit at a UTF-8 character; "" when it is too short to show.
inline std::string field_text(const std::string& text) {
  std::string cut = text.substr(0, kMaxFieldBytes);
  if (cut.size() < text.size()) {
    while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80) {
      cut.pop_back();
    }
    if (!cut.empty() && static_cast<unsigned char>(cut.back()) >= 0xC0) {
      cut.pop_back();
    }
  }
  return cut.size() < 2 ? std::string() : cut;
}

// A resource's button when Discord takes it: a label of 1 to 32 bytes and an https link.
inline bool usable_button(const std::string& label, const std::string& url) {
  if (label.empty() || label.size() > kMaxButtonLabelBytes || url.size() > kMaxButtonUrlBytes) {
    return false;
  }
  if (url.rfind("https://", 0) != 0 || url.size() <= 8) {
    return false;
  }
  for (const char c : url) {
    if (static_cast<unsigned char>(c) <= 0x20 || c == '"' || c == '<' || c == '>') {
      return false;
    }
  }
  return true;
}

inline std::string details_of(const Presence& presence) {
  if (presence.scene == Scene::Playing && !field_text(presence.script.name).empty()) {
    return field_text("KCDS - " + presence.script.name);
  }
  const std::string server = presence.server.empty() ? "a KCDS server" : presence.server;
  switch (presence.scene) {
    case Scene::Starting:
      return "Starting up";
    case Scene::Menu:
      return "In the main menu";
    case Scene::Joining:
      return "Joining " + server;
    case Scene::Playing:
      return "Playing on " + server;
  }
  return {};
}

// The activity object for SET_ACTIVITY.
inline std::string activity_json(const Presence& presence, const std::string& version) {
  std::string json = "{";
  const auto add = [&json](const char* key, const std::string& value) {
    const std::string text = field_text(value);
    if (text.empty()) {
      return false;
    }
    if (json.size() > 1) {
      json += ',';
    }
    json += json_string(key) + ':' + json_string(text);
    return true;
  };
  add("details", details_of(presence));
  // The player count: Discord draws a party's size as "(5 of 32)" after the
  // status line, and only beside one.
  const bool counted = presence.scene == Scene::Playing && presence.max_players > 0;
  if (presence.scene == Scene::Playing) {
    const std::string state = field_text(presence.script.state);
    add("state", state.empty() && counted ? std::string(kPlayersState) : state);
  }
  if (counted) {
    const int places = presence.max_players;
    const int players =
        presence.players < 1 ? 1 : (presence.players > places ? places : presence.players);
    json += ",\"party\":{\"id\":\"kcds\",\"size\":[" + std::to_string(players) + "," +
            std::to_string(places) + "]}";
  }
  if (presence.since > 0) {
    json += ",\"timestamps\":{\"start\":" + std::to_string(presence.since) + "}";
  }
  const bool playing = presence.scene == Scene::Playing;
  const std::string script_logo = playing ? field_text(presence.script.large_image) : std::string();
  json += ",\"assets\":{\"large_image\":" +
          json_string(script_logo.empty() ? std::string(kLogoAsset) : script_logo);
  const std::string script_tooltip =
      playing ? field_text(presence.script.large_text) : std::string();
  const std::string large_text =
      script_tooltip.empty() ? field_text("KCDS " + version) : script_tooltip;
  if (!large_text.empty()) {
    json += ",\"large_text\":" + json_string(large_text);
  }
  if (playing) {
    const std::string small_image = field_text(presence.script.small_image);
    const std::string small_text = field_text(presence.script.small_text);
    if (!small_image.empty()) {
      json += ",\"small_image\":" + json_string(small_image);
      if (!small_text.empty()) {
        json += ",\"small_text\":" + json_string(small_text);
      }
    }
  }
  json += "},\"buttons\":[";
  if (playing && usable_button(presence.script.button_label, presence.script.button_url)) {
    json += "{\"label\":" + json_string(presence.script.button_label) +
            ",\"url\":" + json_string(presence.script.button_url) + "},";
  }
  json +=
      "{\"label\":" + json_string(kWebsiteLabel) + ",\"url\":" + json_string(kWebsiteUrl) + "}]";
  return json + "}";
}

inline std::string handshake_json(const std::string& app_id) {
  return "{\"v\":1,\"client_id\":" + json_string(app_id) + "}";
}

// SET_ACTIVITY for this process; an empty activity clears it.
inline std::string set_activity_json(std::uint32_t pid, const std::string& activity,
                                     std::uint64_t nonce) {
  return "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(pid) +
         ",\"activity\":" + (activity.empty() ? std::string("null") : activity) + "},\"nonce\":\"" +
         std::to_string(nonce) + "\"}";
}

// AUTHORIZE for the KCDS application with the scope identify: Discord asks the
// player once to let KCDS know who they are, and answers with a code the KCDS Master trades for
// their Discord user.
inline std::string authorize_json(const std::string& app_id, std::uint64_t nonce) {
  return "{\"cmd\":\"AUTHORIZE\",\"args\":{\"client_id\":" + json_string(app_id) +
         ",\"scopes\":[\"identify\"]},\"nonce\":\"auth" + std::to_string(nonce) + "\"}";
}

// Whether a frame is Discord's answer to an AUTHORIZE.
inline bool is_authorize_answer(const std::string& body) {
  return body.find("\"cmd\":\"AUTHORIZE\"") != std::string::npos;
}

// The code of an AUTHORIZE answer, "" when Discord refused (the player declined, an error).
inline std::string authorize_code(const std::string& body) {
  if (!is_authorize_answer(body) || body.find("\"evt\":\"ERROR\"") != std::string::npos) {
    return {};
  }
  const std::string key = "\"code\":\"";
  std::size_t at = body.find(key);
  if (at == std::string::npos) {
    return {};
  }
  at += key.size();
  const std::size_t end = body.find('"', at);
  if (end == std::string::npos || end == at || end - at > 256) {
    return {};
  }
  const std::string code = body.substr(at, end - at);
  for (const char c : code) {
    if (c < 33 || c > 126 || c == '\\') {
      return {};
    }
  }
  return code;
}

inline std::vector<std::uint8_t> frame(std::uint32_t op, const std::string& body) {
  std::vector<std::uint8_t> bytes(8 + body.size());
  const auto length = std::uint32_t(body.size());
  for (int i = 0; i < 4; ++i) {
    bytes[i] = std::uint8_t(op >> (8 * i));
    bytes[4 + i] = std::uint8_t(length >> (8 * i));
  }
  std::copy(body.begin(), body.end(), bytes.begin() + 8);
  return bytes;
}

// The signed-in user's id from the READY dispatch ({"cmd":"DISPATCH",...,"evt":"READY",
// "data":{...,"user":{"id":"..."}}}); "" when the frame is not READY.
inline std::string ready_user_id(const std::string& body) {
  if (body.find("\"READY\"") == std::string::npos) {
    return {};
  }
  const std::string key = "\"user\":{\"id\":\"";
  std::size_t at = body.find(key);
  if (at == std::string::npos) {
    return {};
  }
  at += key.size();
  const std::size_t end = body.find('"', at);
  if (end == std::string::npos || end == at || end - at > 24) {
    return {};
  }
  const std::string id = body.substr(at, end - at);
  for (const char c : id) {
    if (c < '0' || c > '9') {
      return {};
    }
  }
  return id;
}

}  // namespace kcds::runtime::discord
