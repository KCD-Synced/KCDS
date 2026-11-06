// Join links: a website, a Discord message or a friend's link opens KCDS straight into
// a server.
//
//   kcds://join/<host:port>   joins that address (a private server, one the Master does not list)
//   kcds://server/<id>        joins the listed server with that Master id
//
// Either may end in "?version=<x.y.z>": the KCDS version the server runs, which the
// launcher starts (installing it when it is not the installed one).
//
// Windows starts the launcher with --uri <link> (the launcher registers the kcds scheme for the
// user on every start). The launcher hands the link to the game client in KCDS_JOIN_ADDRESS or
// KCDS_JOIN_SERVER; the client opens its server browser and joins from there, so a password, a ban
// or a refusal shows in the browser as for a join the player chose. A link only ever names a
// server: it never carries a password or any other setting. Header only, so the launcher and the
// client share one rule.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "kcds/launcher/server_address.hpp"

namespace kcds::launcher::join_link {

inline constexpr const char* kScheme = "kcds";
inline constexpr const char* kAddressVariable = "KCDS_JOIN_ADDRESS";
inline constexpr const char* kServerVariable = "KCDS_JOIN_SERVER";
inline constexpr std::size_t kMaxServerIdBytes = 64;
inline constexpr std::size_t kMaxLinkBytes = 512;

struct JoinLink {
  std::string address;  // host:port, or empty
  std::string server;   // a Master listing id, or empty
  std::string version;  // the KCDS version the server runs, or empty

  bool empty() const {
    return address.empty() && server.empty();
  }
};

// A Master listing id: 1 to 64 letters, digits, '-' and '_'.
inline bool valid_server_id(const std::string& id) {
  if (id.empty() || id.size() > kMaxServerIdBytes) {
    return false;
  }
  for (char c : id) {
    const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    const bool digit = c >= '0' && c <= '9';
    if (!letter && !digit && c != '-' && c != '_') {
      return false;
    }
  }
  return true;
}

// Whether a version is one a link may name: digits separated by dots, at most 32 bytes.
inline bool valid_version(const std::string& version) {
  if (version.empty() || version.size() > 32 || version.front() == '.' || version.back() == '.') {
    return false;
  }
  for (std::size_t i = 0; i < version.size(); ++i) {
    const char c = version[i];
    const bool digit = c >= '0' && c <= '9';
    if (!digit && (c != '.' || version[i - 1] == '.')) {
      return false;
    }
  }
  return true;
}

// The link's server, or nullopt for anything else (another scheme, another action, an invalid
// address or id, a query other than ?version=<x.y.z>, or a fragment). The scheme and the action
// are case-insensitive; one trailing '/' is allowed, as browsers add one.
inline std::optional<JoinLink> parse(const std::string& uri) {
  if (uri.size() > kMaxLinkBytes) {
    return std::nullopt;
  }
  std::string lowered;
  for (char c : uri) {
    lowered.push_back(c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c);
  }
  const std::string prefix = std::string(kScheme) + "://";
  if (lowered.rfind(prefix, 0) != 0) {
    return std::nullopt;
  }
  std::string rest = uri.substr(prefix.size());
  // The version the server runs, the only query a link may carry.
  std::string version;
  const std::size_t query = rest.find('?');
  if (query != std::string::npos) {
    const std::string asked = rest.substr(query + 1);
    const std::string key = "version=";
    if (asked.rfind(key, 0) != 0 || !valid_version(asked.substr(key.size()))) {
      return std::nullopt;
    }
    version = asked.substr(key.size());
    rest = rest.substr(0, query);
    lowered = lowered.substr(0, prefix.size() + query);
  }
  if (!rest.empty() && rest.back() == '/') {
    rest.pop_back();
  }
  const std::size_t slash = rest.find('/');
  if (slash == std::string::npos) {
    return std::nullopt;
  }
  const std::string action = lowered.substr(prefix.size(), slash);
  const std::string target = rest.substr(slash + 1);
  JoinLink link;
  link.version = version;
  if (action == "join") {
    std::string host;
    std::uint16_t port = 0;
    if (!server_address::parse(target, host, port)) {
      return std::nullopt;
    }
    link.address = target;
    return link;
  }
  if (action == "server" && valid_server_id(target)) {
    link.server = target;
    return link;
  }
  return std::nullopt;
}

}  // namespace kcds::launcher::join_link
