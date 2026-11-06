// The game server address, "host:port", from the launcher or the in-game browser.
#pragma once

#include <cstdint>
#include <string>

namespace kcds::game_client {

// Resolves "host:port" to an IPv4 address and a port (Winsock must be started).
bool resolve_server_address(const std::string& text, std::string& ip, std::uint16_t& port);
// The game server address from the launcher, or empty for the in-game server browser.
std::string launcher_server_address();
// The join token the launcher was given for that server (--join-token), or empty. It goes only
// to the server of launcher_server_address().
std::string launcher_join_token();
// The join link the launcher was opened with (kcds): an address or a Master
// listing id, checked again; both empty without one. The browser joins it once it shows.
void launcher_join_link(std::string& address, std::string& server);

}  // namespace kcds::game_client
