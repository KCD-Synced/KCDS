// The game server a player connects to, "host:port". The launcher validates it from its
// deployment configuration and hands it to the game client in KCDS_SERVER_ADDRESS; the client
// parses it again before it connects. Header only, so both share one rule.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace kcds::launcher::server_address {

inline constexpr const char* kVariable = "KCDS_SERVER_ADDRESS";
inline constexpr std::size_t kMaxHostBytes = 253;
// The port a game server listens on unless its owner chose another; a typed address without a
// port joins this one.
inline constexpr std::uint16_t kDefaultPort = 7777;
// The join token (kcds-launcher --join-token) the launcher hands to the game
// client, which sends it only to the server in kVariable, and to the launcher after a self-update.
inline constexpr const char* kJoinTokenVariable = "KCDS_JOIN_TOKEN";

// A join token: 1 to kMaxJoinTokenBytes of letters, digits and - _ . ~ + / = : (base64, base64url,
// JWT and UUID texts fit). The same rule as the transport's JOIN_IDENTITY
// (kcd2s/transport/handshake.hpp join_token_text).
inline constexpr std::size_t kMaxJoinTokenBytes = 255;
inline bool valid_join_token(const std::string& text) {
  if (text.empty() || text.size() > kMaxJoinTokenBytes) {
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

// Host is an IPv4 literal or a DNS name (letters, digits, '-' and '.'); port is 1..65535.
inline bool parse(const std::string& text, std::string& host, std::uint16_t& port) {
  const std::size_t colon = text.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) {
    return false;
  }
  const std::string name = text.substr(0, colon);
  const std::string digits = text.substr(colon + 1);
  if (name.size() > kMaxHostBytes || digits.size() > 5) {
    return false;
  }
  for (char c : name) {
    const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    const bool digit = c >= '0' && c <= '9';
    if (!letter && !digit && c != '-' && c != '.') {
      return false;
    }
  }
  if (name.front() == '.' || name.front() == '-' || name.back() == '.' || name.back() == '-') {
    return false;
  }
  std::uint32_t value = 0;
  for (char c : digits) {
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10 + std::uint32_t(c - '0');
  }
  if (value < 1 || value > 65535) {
    return false;
  }
  host = name;
  port = static_cast<std::uint16_t>(value);
  return true;
}

// The address a player typed, trimmed, with kDefaultPort added when it names only a host
// ("game.example.com" or "192.168.1.10"). Text that already has a port is left as it is; parse()
// still decides whether the result is valid.
inline std::string with_default_port(const std::string& text) {
  const std::size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const std::size_t last = text.find_last_not_of(" \t\r\n");
  std::string address = text.substr(first, last - first + 1);
  if (address.find(':') == std::string::npos) {
    address += ":" + std::to_string(kDefaultPort);
  }
  return address;
}

}  // namespace kcds::launcher::server_address
