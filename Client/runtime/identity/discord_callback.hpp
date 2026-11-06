// The pure parts of the Discord sign-in (discord_login.hpp): Discord's sign-in
// page for a state, and the browser's request back to the game's listener.
#pragma once

#include <string>

namespace kcds::runtime::identity {

inline constexpr char kDiscordRedirect[] = "http://127.0.0.1:47811/discord";

// The parts of "GET /discord?code=...&state=... HTTP/1.1".
struct DiscordCallback {
  std::string code;
  std::string state;
  std::string error;
};

inline std::string url_encoded(const std::string& text) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    const bool plain = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                       (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' ||
                       byte == '~';
    if (plain) {
      out += c;
    } else {
      out += '%';
      out += kHex[byte >> 4];
      out += kHex[byte & 15];
    }
  }
  return out;
}

inline int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

inline std::string url_decoded(const std::string& text) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size() && hex_value(text[i + 1]) >= 0 &&
        hex_value(text[i + 2]) >= 0) {
      out += char(hex_value(text[i + 1]) * 16 + hex_value(text[i + 2]));
      i += 2;
    } else if (text[i] == '+') {
      out += ' ';
    } else {
      out += text[i];
    }
  }
  return out;
}

// The code, state and error of the browser's request back; false when it is another request.
inline bool parse_discord_callback(const std::string& request, DiscordCallback& callback) {
  const std::string prefix = "GET /discord?";
  if (request.rfind(prefix, 0) != 0) {
    return false;
  }
  const std::size_t end = request.find(' ', prefix.size());
  if (end == std::string::npos) {
    return false;
  }
  const std::string query = request.substr(prefix.size(), end - prefix.size());
  std::size_t at = 0;
  while (at <= query.size()) {
    std::size_t next = query.find('&', at);
    if (next == std::string::npos) {
      next = query.size();
    }
    const std::string pair = query.substr(at, next - at);
    const std::size_t equals = pair.find('=');
    if (equals != std::string::npos) {
      const std::string name = pair.substr(0, equals);
      const std::string value = url_decoded(pair.substr(equals + 1));
      if (name == "code") {
        callback.code = value;
      } else if (name == "state") {
        callback.state = value;
      } else if (name == "error") {
        callback.error = value;
      }
    }
    at = next + 1;
  }
  return true;
}

// Discord's sign-in page for this state.
inline std::string discord_login_url(const std::string& app_id, const std::string& state) {
  return "https://discord.com/oauth2/authorize?client_id=" + url_encoded(app_id) +
         "&response_type=code&scope=identify&redirect_uri=" + url_encoded(kDiscordRedirect) +
         "&state=" + url_encoded(state);
}

}  // namespace kcds::runtime::identity
