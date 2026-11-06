#include "server_address.hpp"

#include "../core/log.hpp"

#include <ws2tcpip.h>

#include "kcds/launcher/join_link.hpp"
#include "kcds/launcher/server_address.hpp"

namespace kcds::game_client {

// The game server address, "host:port": from the launcher (KCDS_SERVER_ADDRESS) or picked in the
// in-game browser. A DNS name is resolved to its first IPv4 address. Winsock must be started.
bool resolve_server_address(const std::string& text, std::string& ip, std::uint16_t& port) {
  namespace server_address = kcds::launcher::server_address;
  std::string host;
  if (!server_address::parse(text, host, port)) {
    log("Server address rejected: %s is not host:port", text.c_str());
    return false;
  }
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* found = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || !found) {
    log("Server address %s could not be resolved", host.c_str());
    return false;
  }
  char numeric[INET_ADDRSTRLEN] = {};
  const auto* resolved = reinterpret_cast<const sockaddr_in*>(found->ai_addr);
  const bool converted =
      inet_ntop(AF_INET, &resolved->sin_addr, numeric, sizeof(numeric)) != nullptr;
  freeaddrinfo(found);
  if (!converted) {
    log("Server address %s could not be resolved", host.c_str());
    return false;
  }
  ip = numeric;
  return true;
}
void launcher_join_link(std::string& address, std::string& server) {
  namespace join_link = kcds::launcher::join_link;
  address.clear();
  server.clear();
  char text[300] = {};
  DWORD length = GetEnvironmentVariableA(join_link::kAddressVariable, text, sizeof(text));
  if (length > 0 && length < sizeof(text)) {
    const auto link = join_link::parse(std::string("kcds://join/") + text);
    if (link) {
      address = link->address;
      return;
    }
  }
  length = GetEnvironmentVariableA(join_link::kServerVariable, text, sizeof(text));
  if (length > 0 && length < sizeof(text)) {
    const auto link = join_link::parse(std::string("kcds://server/") + text);
    if (link) {
      server = link->server;
    }
  }
}
// The game server address from the launcher, or empty for the in-game server browser.
std::string launcher_server_address() {
  char text[300] = {};
  const DWORD length =
      GetEnvironmentVariableA(kcds::launcher::server_address::kVariable, text, sizeof(text));
  if (length == 0 || length >= sizeof(text)) {
    return {};
  }
  return text;
}
// The join token the launcher was given (--join-token), or empty.
std::string launcher_join_token() {
  namespace server_address = kcds::launcher::server_address;
  char text[server_address::kMaxJoinTokenBytes + 2] = {};
  const DWORD length =
      GetEnvironmentVariableA(server_address::kJoinTokenVariable, text, sizeof(text));
  if (length == 0 || length >= sizeof(text)) {
    return {};
  }
  const std::string token(text, length);
  return server_address::valid_join_token(token) ? token : std::string();
}

}  // namespace kcds::game_client
