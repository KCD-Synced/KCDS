// The frames of the Discord desktop app's local IPC (the named pipe discord-ipc-0 to 9): a
// little-endian op (0 handshake, 1 frame, 2 close, 3 ping, 4 pong) and length, then that many
// bytes of JSON. Pure; discord_link.cpp talks to Discord with them.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace kcds::loader::discord {

constexpr std::uint32_t kOpHandshake = 0;
constexpr std::uint32_t kOpFrame = 1;
constexpr std::uint32_t kOpClose = 2;
constexpr std::uint32_t kOpPing = 3;
constexpr std::uint32_t kOpPong = 4;
// Discord's frames are small; anything larger is not Discord.
constexpr std::uint32_t kMaxFrameBytes = 64 * 1024;

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

// AUTHORIZE for the KCDS application with the scope identify: Discord asks the player once to let
// KCDS know who they are, and answers with a code the KCDS Master trades for their Discord user.
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

// Whether the activity goes to Discord now: it changed, or it was set long enough ago that it is
// set again (a clip recorder that sets its own activity for the game takes the player's profile
// until then), and never sooner than min_gap after the last one (Discord takes 5 updates in 20
// seconds).
inline bool publish_due(const std::string& wanted, const std::string& published, double now,
                        double last_update, double min_gap, double reassert_after) {
  const bool stale = now - last_update >= reassert_after;
  return (wanted != published || stale) && now - last_update >= min_gap;
}

}  // namespace kcds::loader::discord
