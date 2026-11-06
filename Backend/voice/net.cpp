#include "kcd2s/voice/net.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace kcd2s::voice::net {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
using NativeLength = int;
constexpr int kNoSendSignal = 0;
#else
using NativeSocket = int;
using NativeLength = socklen_t;
constexpr int kNoSendSignal = MSG_NOSIGNAL;
#endif

constexpr int kSendTimeoutMs = 2000;
constexpr int kUdpBufferBytes = 4 * 1024 * 1024;

NativeSocket native(Handle socket) {
  return static_cast<NativeSocket>(socket);
}

Handle handle_of(NativeSocket socket) {
#ifdef _WIN32
  if (socket == INVALID_SOCKET) {
    return kNoSocket;
  }
#else
  if (socket < 0) {
    return kNoSocket;
  }
#endif
  return static_cast<Handle>(socket);
}

void set_blocking(Handle socket, bool blocking) {
#ifdef _WIN32
  u_long mode = blocking ? 0 : 1;
  ioctlsocket(native(socket), FIONBIO, &mode);
#else
  const int flags = fcntl(native(socket), F_GETFL, 0);
  fcntl(native(socket), F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
#endif
}

void set_send_timeout(Handle socket, int timeout_ms) {
#ifdef _WIN32
  const DWORD value = static_cast<DWORD>(timeout_ms);
#else
  timeval value{};
  value.tv_sec = timeout_ms / 1000;
  value.tv_usec = (timeout_ms % 1000) * 1000;
#endif
  setsockopt(native(socket), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&value),
             sizeof(value));
}

void prepare_tcp(Handle socket) {
  const int on = 1;
  setsockopt(native(socket), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on),
             sizeof(on));
  set_send_timeout(socket, kSendTimeoutMs);
}

// Whether the socket can be read (or written) within the time.
bool wait_for(Handle socket, bool for_writing, int timeout_ms) {
  fd_set set;
  FD_ZERO(&set);
  FD_SET(native(socket), &set);
  timeval timeout{};
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = (timeout_ms % 1000) * 1000;
  const int nfds = static_cast<int>(native(socket)) + 1;
  const int ready = for_writing ? select(nfds, nullptr, &set, nullptr, &timeout)
                                : select(nfds, &set, nullptr, nullptr, &timeout);
  return ready > 0;
}

sockaddr_in address_of(const std::string& ip, std::uint16_t host_port) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(host_port);
  if (ip.empty() || inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) {
    address.sin_addr.s_addr = htonl(INADDR_ANY);
  }
  return address;
}

std::uint16_t port_of(Handle socket) {
  sockaddr_in address{};
  NativeLength size = sizeof(address);
  if (getsockname(native(socket), reinterpret_cast<sockaddr*>(&address), &size) != 0) {
    return 0;
  }
  return ntohs(address.sin_port);
}

}  // namespace

void start_sockets() {
#ifdef _WIN32
  static std::once_flag once;
  std::call_once(once, [] {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  });
#endif
}

void close_socket(Handle socket) {
  if (socket == kNoSocket) {
    return;
  }
#ifdef _WIN32
  closesocket(native(socket));
#else
  close(native(socket));
#endif
}

std::uint16_t host_to_network_port(std::uint16_t port) {
  return htons(port);
}

std::optional<std::uint32_t> resolve_ipv4(const std::string& host) {
  start_sockets();
  addrinfo hints{};
  hints.ai_family = AF_INET;
  addrinfo* found = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || found == nullptr) {
    return std::nullopt;
  }
  const std::uint32_t ip = reinterpret_cast<sockaddr_in*>(found->ai_addr)->sin_addr.s_addr;
  freeaddrinfo(found);
  return ip;
}

bool split_host_port(const std::string& text, std::string& host, std::uint16_t& port) {
  const std::size_t colon = text.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size() ||
      text.size() - colon - 1 > 5) {
    return false;
  }
  const std::string digits = text.substr(colon + 1);
  if (digits.find_first_not_of("0123456789") != std::string::npos) {
    return false;
  }
  const unsigned long value = std::stoul(digits);
  if (value == 0 || value > 65535) {
    return false;
  }
  host = text.substr(0, colon);
  port = static_cast<std::uint16_t>(value);
  return true;
}

Handle tcp_listen(const std::string& bind_ip, std::uint16_t port, std::uint16_t& bound_port) {
  start_sockets();
  const Handle socket = handle_of(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  if (socket == kNoSocket) {
    return kNoSocket;
  }
#ifndef _WIN32
  const int on = 1;
  setsockopt(native(socket), SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif
  const sockaddr_in address = address_of(bind_ip, port);
  if (bind(native(socket), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
      listen(native(socket), 16) != 0) {
    close_socket(socket);
    return kNoSocket;
  }
  bound_port = port_of(socket);
  return socket;
}

Handle tcp_accept(Handle listener, int timeout_ms) {
  if (!wait_for(listener, false, timeout_ms)) {
    return kNoSocket;
  }
  const Handle socket = handle_of(accept(native(listener), nullptr, nullptr));
  if (socket != kNoSocket) {
    prepare_tcp(socket);
  }
  return socket;
}

Handle tcp_connect(std::uint32_t ip, std::uint16_t port, int timeout_ms) {
  start_sockets();
  const Handle socket = handle_of(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  if (socket == kNoSocket) {
    return kNoSocket;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = ip;
  set_blocking(socket, false);
  connect(native(socket), reinterpret_cast<const sockaddr*>(&address), sizeof(address));
  int problem = 1;
  NativeLength problem_size = sizeof(problem);
  const bool connected = wait_for(socket, true, timeout_ms) &&
                         getsockopt(native(socket), SOL_SOCKET, SO_ERROR,
                                    reinterpret_cast<char*>(&problem), &problem_size) == 0 &&
                         problem == 0;
  if (!connected) {
    close_socket(socket);
    return kNoSocket;
  }
  set_blocking(socket, true);
  prepare_tcp(socket);
  return socket;
}

int tcp_read(Handle socket, std::uint8_t* data, std::size_t size, int timeout_ms) {
  if (!wait_for(socket, false, timeout_ms)) {
    return 0;
  }
  const int got = recv(native(socket), reinterpret_cast<char*>(data), static_cast<int>(size), 0);
  return got > 0 ? got : -1;
}

bool tcp_write(Handle socket, const std::uint8_t* data, std::size_t size) {
  std::size_t done = 0;
  while (done < size) {
    const int sent = send(native(socket), reinterpret_cast<const char*>(data) + done,
                          static_cast<int>(size - done), kNoSendSignal);
    if (sent <= 0) {
      return false;
    }
    done += static_cast<std::size_t>(sent);
  }
  return true;
}

Handle udp_open(const std::string& bind_ip, std::uint16_t port, std::uint16_t& bound_port) {
  start_sockets();
  const Handle socket = handle_of(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
  if (socket == kNoSocket) {
    return kNoSocket;
  }
  const sockaddr_in address = address_of(bind_ip, port);
  if (bind(native(socket), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    close_socket(socket);
    return kNoSocket;
  }
  setsockopt(native(socket), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&kUdpBufferBytes),
             sizeof(kUdpBufferBytes));
  setsockopt(native(socket), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&kUdpBufferBytes),
             sizeof(kUdpBufferBytes));
  bound_port = port_of(socket);
  return socket;
}

void udp_send(Handle socket, const Address& to, const std::uint8_t* data, std::size_t size) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = to.ip;
  address.sin_port = to.port;
  sendto(native(socket), reinterpret_cast<const char*>(data), static_cast<int>(size), kNoSendSignal,
         reinterpret_cast<const sockaddr*>(&address), sizeof(address));
}

int udp_recv(Handle socket, std::uint8_t* data, std::size_t size, Address& from, int timeout_ms) {
  if (!wait_for(socket, false, timeout_ms)) {
    return 0;
  }
  sockaddr_in address{};
  NativeLength address_size = sizeof(address);
  const int got = recvfrom(native(socket), reinterpret_cast<char*>(data), static_cast<int>(size), 0,
                           reinterpret_cast<sockaddr*>(&address), &address_size);
  if (got <= 0) {
    return 0;
  }
  from.ip = address.sin_addr.s_addr;
  from.port = address.sin_port;
  return got;
}

}  // namespace kcd2s::voice::net
