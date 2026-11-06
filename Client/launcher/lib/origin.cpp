#include "kcds/launcher/origin.hpp"

#include <cstddef>
#include <string>

// The origin rule the update feed and the crash-reporting DSN share (one rule for both endpoints).
namespace kcds::launcher::feed {

namespace {

constexpr std::size_t kMaxOriginLength = 2048;

std::string to_lower_ascii(const std::string& value) {
  std::string lower = value;
  for (char& c : lower) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return lower;
}

bool is_ascii_alnum(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool is_hex_digit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool is_loopback_host(const std::string& host) {
  return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}

// A bracketed IPv6 literal: "[" hex digits, ':' and '.' "]".
bool is_ipv6_literal(const std::string& host) {
  if (host.size() < 3 || host.front() != '[' || host.back() != ']') {
    return false;
  }
  for (std::size_t i = 1; i + 1 < host.size(); ++i) {
    char c = host[i];
    if (!is_hex_digit(c) && c != ':' && c != '.') {
      return false;
    }
  }
  return true;
}

// A DNS name or dotted IPv4 address: dot-separated, non-empty labels of
// letters, digits and '-'.
bool is_host_name(const std::string& host) {
  if (host.empty() || host.size() > 253) {
    return false;
  }
  std::size_t label_length = 0;
  for (char c : host) {
    if (c == '.') {
      if (label_length == 0) {
        return false;
      }
      label_length = 0;
      continue;
    }
    if (!is_ascii_alnum(c) && c != '-') {
      return false;
    }
    ++label_length;
  }
  return label_length != 0;
}

// An optional ":port" suffix with a port of 1..65535.
bool is_port_suffix(const std::string& suffix) {
  if (suffix.empty()) {
    return true;
  }
  if (suffix.front() != ':' || suffix.size() < 2 || suffix.size() > 6) {
    return false;
  }
  long port = 0;
  for (std::size_t i = 1; i < suffix.size(); ++i) {
    char c = suffix[i];
    if (c < '0' || c > '9') {
      return false;
    }
    port = port * 10 + (c - '0');
  }
  return port >= 1 && port <= 65535;
}

// Parses "scheme://host[:port][/]" into a normalised origin. Anything else --
// user info, a path, a query, a fragment, percent-encoding or whitespace -- is
// refused because none of it can occur in a bare origin.
bool parse_origin(const std::string& origin, bool allow_http_loopback, std::string& normalized) {
  if (origin.size() > kMaxOriginLength) {
    return false;
  }

  std::string scheme;
  std::string remainder;
  if (origin.starts_with("https://")) {
    scheme = "https";
    remainder = origin.substr(8);
  } else if (origin.starts_with("http://")) {
    scheme = "http";
    remainder = origin.substr(7);
  } else {
    return false;
  }

  if (!remainder.empty() && remainder.back() == '/') {
    remainder.pop_back();
  }
  if (remainder.empty()) {
    return false;
  }
  for (char c : remainder) {
    bool allowed = is_ascii_alnum(c) || c == '.' || c == '-' || c == ':' || c == '[' || c == ']';
    if (!allowed) {
      return false;
    }
  }

  std::string host;
  std::string port_suffix;
  if (remainder.front() == '[') {
    std::size_t close = remainder.find(']');
    if (close == std::string::npos) {
      return false;
    }
    host = remainder.substr(0, close + 1);
    port_suffix = remainder.substr(close + 1);
    if (!is_ipv6_literal(host)) {
      return false;
    }
  } else {
    std::size_t colon = remainder.find(':');
    host = remainder.substr(0, colon);
    if (colon != std::string::npos) {
      port_suffix = remainder.substr(colon);
    }
    if (!is_host_name(host)) {
      return false;
    }
  }
  if (!is_port_suffix(port_suffix)) {
    return false;
  }

  host = to_lower_ascii(host);
  if (scheme == "http") {
    if (!allow_http_loopback || !is_loopback_host(host)) {
      return false;
    }
  }

  normalized = scheme + "://" + host + port_suffix;
  return true;
}

}  // namespace

bool normalize_origin(const std::string& origin, bool allow_http_loopback,
                      std::string& normalized_origin) {
  return parse_origin(origin, allow_http_loopback, normalized_origin);
}

}  // namespace kcds::launcher::feed
