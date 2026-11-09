#pragma once
// Master-server registration of the local KCDS server: whether and how the server
// lists itself in the public server browser. Read from the environment; portable so the parser is
// covered by the Linux unit tests.
//
//   KCDS_MASTER_URL             whether the server lists itself: unset or "off" keeps it private
//                               (the default), "on" lists it on https://master.kcdsynced.com, and
//                               a URL lists it on that master. Plain http:// is accepted only for
//                               a loopback master (development).
//   KCDS_SERVER_NAME            name shown in the browser, at most 64 bytes (default "KCDS server")
//   KCDS_SERVER_PUBLIC_ADDRESS  host:port players dial. Default: only the UDP port is sent and the
//                               master lists the server under the address the heartbeat came
//                               from. Required when the server binds a loopback address.
//   KCDS_SERVER_IMAGE_URL       optional https URL of the server's card art in the browser, at
//                               most 512 bytes; the master validates it again.
//   KCDS_SERVER_DESCRIPTION     optional short description shown when a player opens the server's
//                               row, at most 600 bytes (about 300 characters)
//   KCDS_SERVER_TAGS            optional tags, comma separated, from the fixed list
//                               (kKnownServerTags: Roleplay, PvP, ...); at most 8
//   KCDS_SERVER_WEBSITE         optional http or https URL the browser offers to open, at most
//                               200 bytes
//
// A server bound to a loopback address without KCDS_SERVER_PUBLIC_ADDRESS stays private, because
// nobody else could reach it.
#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "kcd2s/platform/server_browser.hpp"
#include "kcds/launcher/server_address.hpp"

namespace kcd2s::local::master {

inline constexpr const char* kDefaultMasterUrl = "https://master.kcdsynced.com";
inline constexpr const char* kDefaultServerName = "KCDS server";
inline constexpr std::size_t kMaxServerNameBytes = 64;
inline constexpr std::size_t kMaxImageUrlBytes = 512;
inline constexpr std::size_t kMaxDescriptionBytes = 600;
inline constexpr std::size_t kMaxWebsiteBytes = 200;
inline constexpr std::size_t kMaxTags = 8;

struct RegistrationSettings {
  bool enabled = false;
  std::string master_url;  // without a trailing slash
  std::string name = kDefaultServerName;
  std::string id;       // stable per host and port, e.g. "kcds-7777"
  std::string address;  // "host:port" or ":port"
  std::string image;    // optional https card art URL
  std::string description;
  std::vector<std::string> tags;  // from kKnownServerTags, in its spelling
  std::string website;            // optional http or https URL
  bool password = false;          // KCDS_SERVER_PASSWORD is set: the browser shows a lock
  std::string reason;             // why registration is off, for the console
};

using SettingLookup = std::function<std::optional<std::string>(const char* name)>;

namespace detail {

inline bool loopback_host(const std::string& host) {
  return host == "localhost" || host == "::1" || host.rfind("127.", 0) == 0 ||
         host.rfind("[::1]", 0) == 0;
}

// https://<host>[:port][/path] or http://<loopback>[:port][/path]; no credentials, query or
// fragment.
inline bool acceptable_master_url(const std::string& url) {
  const bool secure = url.rfind("https://", 0) == 0;
  const bool plain = url.rfind("http://", 0) == 0;
  if (!secure && !plain) {
    return false;
  }
  const std::string rest = url.substr(secure ? 8 : 7);
  const std::string authority = rest.substr(0, rest.find('/'));
  if (authority.empty() || url.find_first_of("@?# \t") != std::string::npos) {
    return false;
  }
  std::string host = authority;
  const auto colon = host.rfind(':');
  if (colon != std::string::npos && host.find(']') == std::string::npos) {
    host = host.substr(0, colon);
  }
  return secure || loopback_host(host);
}

inline bool printable(const std::string& text) {
  for (char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7f) {
      return false;
    }
  }
  return true;
}

}  // namespace detail

// The server's name as the browser lists it, from KCDS_SERVER_NAME whether the server is listed or
// not (the players' watermarks show it): the default when it is unset or invalid.
template <class Lookup>
std::string server_name(Lookup&& lookup) {
  if (const auto name = lookup("KCDS_SERVER_NAME")) {
    if (!name->empty() && name->size() <= kMaxServerNameBytes && detail::printable(*name)) {
      return *name;
    }
  }
  return kDefaultServerName;
}

namespace detail {

// https://<host>[:port]/<path> without credentials or characters that could leave a CSS url().
inline bool acceptable_image_url(const std::string& url) {
  if (url.size() > kMaxImageUrlBytes || url.rfind("https://", 0) != 0) {
    return false;
  }
  if (url.find_first_of(" \"'()<>\\`@") != std::string::npos || !printable(url)) {
    return false;
  }
  const std::string rest = url.substr(8);
  const std::string authority = rest.substr(0, rest.find_first_of("/?#"));
  return !authority.empty() && authority.front() != ':';
}

// http(s)://<host>[:port][/path] without credentials or characters that could leave an HTML
// attribute: the browser opens it in the player's own web browser.
inline bool acceptable_website(const std::string& url) {
  const bool secure = url.rfind("https://", 0) == 0;
  const bool plain = url.rfind("http://", 0) == 0;
  if (url.size() > kMaxWebsiteBytes || (!secure && !plain)) {
    return false;
  }
  if (url.find_first_of(" \"'()<>\\`@") != std::string::npos || !printable(url)) {
    return false;
  }
  const std::string rest = url.substr(secure ? 8 : 7);
  const std::string authority = rest.substr(0, rest.find_first_of("/?#"));
  return !authority.empty() && authority.front() != ':';
}

inline std::string trimmed(const std::string& text) {
  const auto first = text.find_first_not_of(" \t");
  if (first == std::string::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(" \t");
  return text.substr(first, last - first + 1);
}

inline char lower(char c) {
  return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
}

// The list's spelling of `tag` (matched without regard to case), or empty when it is not on the
// fixed list.
inline std::string known_tag(const std::string& tag) {
  for (const char* known : platform::kKnownServerTags) {
    const std::string name = known;
    bool same = name.size() == tag.size();
    for (std::size_t i = 0; same && i < name.size(); ++i) {
      same = lower(name[i]) == lower(tag[i]);
    }
    if (same) {
      return name;
    }
  }
  return {};
}

}  // namespace detail

// The tags in a comma separated list: each known one once, in the list's spelling. Unknown ones
// go to `problems` (they would not be listed).
inline std::vector<std::string> server_tags(const std::string& text,
                                            std::vector<std::string>& problems) {
  std::vector<std::string> tags;
  std::size_t start = 0;
  while (start <= text.size()) {
    const auto comma = text.find(',', start);
    const std::string word = detail::trimmed(
        text.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
    if (!word.empty()) {
      const std::string known = detail::known_tag(word);
      if (known.empty()) {
        problems.push_back("Ignoring unknown server tag \"" + word +
                           "\" (tags come from the fixed list: Roleplay, PvP, PvE, Hardcore, "
                           "Whitelist, Events, Racing, Siege, Survival, Freeroam, Economy, "
                           "Beginner-friendly, Voice chat, 18+)");
      } else if (std::find(tags.begin(), tags.end(), known) == tags.end()) {
        if (tags.size() < kMaxTags) {
          tags.push_back(known);
        } else {
          problems.push_back("Ignoring server tag \"" + known + "\": at most 8 tags");
        }
      }
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  return tags;
}

// Reads the registration settings through `lookup` for a server bound to bind_ip:port. Invalid
// values turn registration off and add a message to `problems`.
inline RegistrationSettings read_registration_settings(const SettingLookup& lookup,
                                                       const std::string& bind_ip,
                                                       std::uint16_t port,
                                                       std::vector<std::string>& problems) {
  RegistrationSettings settings;
  settings.id = "kcds-" + std::to_string(port);

  // A server is listed only when its owner turns the listing on.
  std::string url;
  if (const auto value = lookup("KCDS_MASTER_URL")) {
    url = *value;
  }
  if (url.empty()) {
    settings.reason = "master is not set (\"master\": \"on\" in server.json lists the server)";
    return settings;
  }
  if (url == "off") {
    settings.reason = "KCDS_MASTER_URL=off";
    return settings;
  }
  if (url == "on") {
    url = kDefaultMasterUrl;
  }
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }
  if (!detail::acceptable_master_url(url)) {
    problems.push_back("Ignoring invalid KCDS_MASTER_URL=" + url +
                       " (https:// required; http:// only for a loopback master)");
    settings.reason = "invalid KCDS_MASTER_URL";
    return settings;
  }
  settings.master_url = url;

  if (const auto name = lookup("KCDS_SERVER_NAME")) {
    if (name->empty() || name->size() > kMaxServerNameBytes || !detail::printable(*name)) {
      problems.push_back("Ignoring invalid KCDS_SERVER_NAME (1-64 printable bytes)");
    } else {
      settings.name = *name;
    }
  }

  if (const auto password = lookup("KCDS_SERVER_PASSWORD")) {
    settings.password = !password->empty();
  }

  if (const auto image = lookup("KCDS_SERVER_IMAGE_URL")) {
    if (detail::acceptable_image_url(*image)) {
      settings.image = *image;
    } else {
      problems.push_back("Ignoring invalid KCDS_SERVER_IMAGE_URL (https URL, at most 512 bytes)");
    }
  }

  if (const auto description = lookup("KCDS_SERVER_DESCRIPTION")) {
    const std::string text = detail::trimmed(*description);
    if (text.size() <= kMaxDescriptionBytes && detail::printable(text)) {
      settings.description = text;
    } else {
      problems.push_back(
          "Ignoring invalid KCDS_SERVER_DESCRIPTION (printable text, at most 600 bytes)");
    }
  }

  if (const auto tags = lookup("KCDS_SERVER_TAGS")) {
    settings.tags = server_tags(*tags, problems);
  }

  if (const auto website = lookup("KCDS_SERVER_WEBSITE")) {
    if (detail::acceptable_website(*website)) {
      settings.website = *website;
    } else if (!website->empty()) {
      problems.push_back(
          "Ignoring invalid KCDS_SERVER_WEBSITE (http or https URL, at most 200 bytes)");
    }
  }

  if (const auto address = lookup("KCDS_SERVER_PUBLIC_ADDRESS")) {
    std::string host;
    std::uint16_t public_port = 0;
    if (!kcds::launcher::server_address::parse(*address, host, public_port)) {
      problems.push_back("Ignoring invalid KCDS_SERVER_PUBLIC_ADDRESS=" + *address +
                         " (host:port required)");
      settings.reason = "invalid KCDS_SERVER_PUBLIC_ADDRESS";
      return settings;
    }
    settings.address = *address;
  } else if (detail::loopback_host(bind_ip)) {
    settings.reason = "bound to " + bind_ip + " without KCDS_SERVER_PUBLIC_ADDRESS";
    return settings;
  } else {
    settings.address = ":" + std::to_string(port);
  }
  settings.enabled = true;
  return settings;
}

}  // namespace kcd2s::local::master
