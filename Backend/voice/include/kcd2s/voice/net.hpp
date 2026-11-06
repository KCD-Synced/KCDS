// The few socket calls the voice link and kcds-voice need, on Windows and Linux. IPv4. Addresses
// and ports in Address are in network order, as the game host's socket gives them.
#ifndef KCD2S_VOICE_NET_HPP
#define KCD2S_VOICE_NET_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "kcd2s/voice/voice_server.hpp"

namespace kcd2s::voice::net {

using Handle = std::intptr_t;
constexpr Handle kNoSocket = -1;

// Safe to call often: starts the socket library where it needs that.
void start_sockets();
void close_socket(Handle socket);

// A port as Address holds it.
std::uint16_t host_to_network_port(std::uint16_t port);
// The IPv4 address of a name or dotted address.
std::optional<std::uint32_t> resolve_ipv4(const std::string& host);
// 'host:port' into its parts. False when the port is missing or out of range.
bool split_host_port(const std::string& text, std::string& host, std::uint16_t& port);

// TCP. `bound_port` tells the port when asked for 0. tcp_read: bytes, 0 for nothing within the
// time, -1 for a closed connection.
Handle tcp_listen(const std::string& bind_ip, std::uint16_t port, std::uint16_t& bound_port);
Handle tcp_accept(Handle listener, int timeout_ms);
Handle tcp_connect(std::uint32_t ip, std::uint16_t port, int timeout_ms);
int tcp_read(Handle socket, std::uint8_t* data, std::size_t size, int timeout_ms);
bool tcp_write(Handle socket, const std::uint8_t* data, std::size_t size);

// UDP. udp_recv: bytes, 0 for nothing within the time or a failed read.
Handle udp_open(const std::string& bind_ip, std::uint16_t port, std::uint16_t& bound_port);
void udp_send(Handle socket, const Address& to, const std::uint8_t* data, std::size_t size);
int udp_recv(Handle socket, std::uint8_t* data, std::size_t size, Address& from, int timeout_ms);

}  // namespace kcd2s::voice::net

#endif
