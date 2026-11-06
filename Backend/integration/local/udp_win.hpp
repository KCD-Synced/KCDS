#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <functional>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "kcd2s/transport/datagram.hpp"
#include "metrics/counters.hpp"
#include "netsim/impairment.hpp"

namespace kcd2s::local {
using transport::Datagram;
struct Winsock {
  Winsock() {
    WSADATA d{};
    if (WSAStartup(MAKEWORD(2, 2), &d))
      throw std::runtime_error("WSAStartup failed");
  }
  ~Winsock() {
    WSACleanup();
  }
};
inline sockaddr_in address(const char* ip, unsigned short port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  if (inet_pton(AF_INET, ip, &a.sin_addr) != 1)
    throw std::runtime_error("Expected numeric IPv4 address");
  return a;
}
inline std::uint64_t key(const sockaddr_in& a) {
  return (std::uint64_t(a.sin_addr.s_addr) << 16) | a.sin_port;
}
class UdpSocket {
 public:
  explicit UdpSocket(const sockaddr_in& local) {
    fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd_ == INVALID_SOCKET)
      throw std::runtime_error("socket failed");
    u_long one = 1;
    if (bind(fd_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) ||
        ioctlsocket(fd_, FIONBIO, &one)) {
      closesocket(fd_);
      fd_ = INVALID_SOCKET;
      throw std::runtime_error("UDP bind/nonblocking setup failed");
    }
  }
  ~UdpSocket() {
    if (reader_.joinable()) {
      reader_stop_.store(true, std::memory_order_relaxed);
      reader_.join();
    }
    if (fd_ != INVALID_SOCKET)
      closesocket(fd_);
  }
  // A datagram this says is taken (voice) goes no further: on the reader thread when there is one.
  // Set before start_reader.
  void set_datagram_tap(std::function<bool(const sockaddr_in&, const Datagram&)> tap) {
    datagram_tap_ = std::move(tap);
  }
  // Read the socket on a thread of its own from now on: it takes every datagram off the OS buffer
  // as it arrives and keeps up to `limit` of them (more are dropped and counted, as the OS would
  // drop them); recv() hands them out on the caller's thread. A host then spends its tick on the
  // datagrams, not on one recvfrom call each. A receive error of the thread is thrown by recv().
  void start_reader(std::size_t limit) {
    if (reader_.joinable())
      return;
    reader_limit_ = limit;
    reader_ = std::thread([this] { read_loop(); });
  }
  // Datagrams the reader dropped because `limit` of them were waiting.
  std::uint64_t reader_drops() const {
    std::lock_guard<std::mutex> lock(arrived_mutex_);
    return reader_drops_;
  }
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;
  // Ask the OS for larger receive and send buffers. Datagrams that arrive while the receive
  // buffer is full are dropped by the OS without any signal, so a host that drains its socket
  // once per tick needs room for a whole tick of input. Returns false if the OS refused.
  bool set_buffer_bytes(int receive, int send) {
    const bool receive_ok =
        setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receive),
                   sizeof(receive)) == 0;
    const bool send_ok = setsockopt(fd_, SOL_SOCKET, SO_SNDBUF,
                                    reinterpret_cast<const char*>(&send), sizeof(send)) == 0;
    return receive_ok && send_ok;
  }
  // Buffer sizes the OS reports for this socket (0 if the query fails).
  int receive_buffer_bytes() const {
    return socket_option(SO_RCVBUF);
  }
  int send_buffer_bytes() const {
    return socket_option(SO_SNDBUF);
  }
  // Measurement tools only (kcd2s-load-probe): emulate random loss and a fixed one-way delay in
  // both directions of this socket. Inactive settings remove the impairment. Delayed datagrams
  // move on the next send() or recv() call. Traffic counters count only datagrams that crossed
  // the emulated path.
  void set_impairment(const netsim::ImpairmentSettings& settings) {
    if (!settings.active()) {
      outbound_.reset();
      inbound_.reset();
      return;
    }
    outbound_ = std::make_unique<netsim::ImpairedQueue<Pending>>(settings);
    netsim::ImpairmentSettings inbound_settings = settings;
    inbound_settings.seed = settings.seed ^ 0x5bd1e9955bd1e995ull;
    inbound_ = std::make_unique<netsim::ImpairedQueue<Pending>>(inbound_settings);
  }
  netsim::ImpairmentCounters outbound_impairment() const {
    return outbound_ ? outbound_->counters() : netsim::ImpairmentCounters{};
  }
  netsim::ImpairmentCounters inbound_impairment() const {
    return inbound_ ? inbound_->counters() : netsim::ImpairmentCounters{};
  }
  // Returns false when the OS send buffer was full and the datagram was dropped (would block).
  // Safe to call from several threads at once (the server flushes its clients in parallel).
  bool send(const sockaddr_in& to, const Datagram& d) {
    if (d.size() > 65507)
      throw std::runtime_error("UDP payload too large");
    if (outbound_) {
      std::lock_guard<std::mutex> lock(outbound_mutex_);
      outbound_->offer(impairment_clock(), Pending{to, d});
      flush_outbound();
      return true;
    }
    return send_now(to, d);
  }
  bool recv(sockaddr_in& from, Datagram& out) {
    if (!inbound_) {
      if (!next_arrived(from, out))
        return false;
      traffic_.record_received(out.size());
      return true;
    }
    {
      std::lock_guard<std::mutex> lock(outbound_mutex_);
      flush_outbound();
    }
    const double now = impairment_clock();
    Pending arrived;
    while (next_arrived(arrived.address, arrived.data)) {
      inbound_->offer(now, std::move(arrived));
      arrived = Pending{};
    }
    Pending due;
    if (!inbound_->take_due(now, due))
      return false;
    from = due.address;
    out = std::move(due.data);
    traffic_.record_received(out.size());
    return true;
  }
  // Cumulative UDP payload counters of this socket (all peers, including handshakes).
  const metrics::TrafficCounters& traffic() const {
    return traffic_;
  }
  // Best-effort send for shutdown paths: one non-blocking sendto on the existing socket, no heap
  // allocation, no exceptions. Returns true when the OS accepted the whole datagram.
  bool try_send(const sockaddr_in& to, const Datagram& d) noexcept {
    if (fd_ == INVALID_SOCKET || d.empty() || d.size() > 65507)
      return false;
    const int n = sendto(fd_, reinterpret_cast<const char*>(d.data()), static_cast<int>(d.size()),
                         0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    return n == static_cast<int>(d.size());
  }
  // The bound local port (useful after binding port 0 for an ephemeral port).
  unsigned short local_port() const {
    sockaddr_in local{};
    int len = sizeof(local);
    if (getsockname(fd_, reinterpret_cast<sockaddr*>(&local), &len) == SOCKET_ERROR)
      throw std::runtime_error("getsockname failed");
    return ntohs(local.sin_port);
  }

 private:
  struct Pending {
    sockaddr_in address{};
    Datagram data;
  };
  static double impairment_clock() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double>(now).count();
  }
  // sendto itself is safe on one socket from several threads; the counters are not.
  // A send error a moment of network trouble causes: the datagram is lost, like any other, and the
  // session goes on (a player's game lost its server for good on one such error).
  static bool passing_send_error(int code) {
    switch (code) {
      case WSAENOBUFS:
      case WSAENETDOWN:
      case WSAENETUNREACH:
      case WSAEHOSTUNREACH:
      case WSAENETRESET:
      case WSAECONNRESET:
      case WSAEADDRNOTAVAIL:
      case WSAEINTR:
        return true;
      default:
        return false;
    }
  }
  bool send_now(const sockaddr_in& to, const Datagram& d) {
    int n = sendto(fd_, reinterpret_cast<const char*>(d.data()), static_cast<int>(d.size()), 0,
                   reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    if (n == SOCKET_ERROR) {
      const int code = WSAGetLastError();
      if (code == WSAEWOULDBLOCK) {
        std::lock_guard<std::mutex> lock(traffic_mutex_);
        traffic_.record_would_block();
        return false;
      }
      if (passing_send_error(code)) {
        std::lock_guard<std::mutex> lock(traffic_mutex_);
        traffic_.record_dropped();
        return false;
      }
      throw std::runtime_error("UDP send failed (WSA error " + std::to_string(code) + ")");
    }
    std::lock_guard<std::mutex> lock(traffic_mutex_);
    traffic_.record_sent(d.size());
    return true;
  }
  bool receive_now(sockaddr_in& from, Datagram& out) {
    char bytes[65536];
    int len = sizeof(from);
    int n = recvfrom(fd_, bytes, sizeof(bytes), 0, reinterpret_cast<sockaddr*>(&from), &len);
    if (n == SOCKET_ERROR) {
      int e = WSAGetLastError();
      if (e == WSAEWOULDBLOCK || e == WSAECONNRESET)
        return false;
      throw std::runtime_error("UDP recv failed");
    }
    out.assign(bytes, bytes + n);
    return true;
  }
  // The next datagram: from the reader when it runs, otherwise straight from the socket.
  bool next_arrived(sockaddr_in& from, Datagram& out) {
    if (!reader_.joinable())
      return receive_now(from, out);
    if (taken_next_ == taken_.size()) {
      taken_.clear();
      taken_next_ = 0;
      std::lock_guard<std::mutex> lock(arrived_mutex_);
      if (reader_error_)
        std::rethrow_exception(reader_error_);
      taken_.swap(arrived_);
      if (taken_.empty())
        return false;
    }
    Pending& next = taken_[taken_next_++];
    from = next.address;
    out = std::move(next.data);
    return true;
  }
  void read_loop() {
    WSAPOLLFD poll{};
    poll.fd = fd_;
    poll.events = POLLRDNORM;
    std::vector<Pending> batch;
    while (!reader_stop_.load(std::memory_order_relaxed)) {
      poll.revents = 0;
      const int ready = WSAPoll(&poll, 1, 20);  // wakes up to see a stop request
      if (ready < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      if (ready == 0)
        continue;
      try {
        Pending arrived;
        while (batch.size() < 256 && receive_now(arrived.address, arrived.data)) {
          if (datagram_tap_ && datagram_tap_(arrived.address, arrived.data)) {
            continue;
          }
          batch.push_back(std::move(arrived));
          arrived = Pending{};
        }
      } catch (...) {
        std::lock_guard<std::mutex> lock(arrived_mutex_);
        reader_error_ = std::current_exception();
        return;
      }
      if (batch.empty())
        continue;
      std::lock_guard<std::mutex> lock(arrived_mutex_);
      for (Pending& arrived : batch) {
        if (arrived_.size() >= reader_limit_) {
          reader_drops_ += 1;
          continue;
        }
        arrived_.push_back(std::move(arrived));
      }
      batch.clear();
    }
  }
  void flush_outbound() {
    const double now = impairment_clock();
    Pending due;
    while (outbound_->take_due(now, due)) {
      send_now(due.address, due.data);
    }
  }
  int socket_option(int name) const {
    int value = 0;
    int length = sizeof(value);
    if (getsockopt(fd_, SOL_SOCKET, name, reinterpret_cast<char*>(&value), &length) != 0)
      return 0;
    return value;
  }

  SOCKET fd_ = INVALID_SOCKET;
  // Sends come from several threads at once (Server::tick flushes its clients in parallel): the
  // traffic counters and the outbound impairment queue are shared. The queue's lock is taken
  // first when both are held. Receives and traffic() run on the host thread between ticks, never
  // during those sends.
  std::mutex traffic_mutex_;
  std::mutex outbound_mutex_;
  metrics::TrafficCounters traffic_;
  std::unique_ptr<netsim::ImpairedQueue<Pending>> outbound_;
  std::unique_ptr<netsim::ImpairedQueue<Pending>> inbound_;
  // The reader thread (start_reader) and what it hands to recv(): `arrived_` is filled by the
  // thread under the lock; recv() swaps it into `taken_` and hands those out one by one.
  std::function<bool(const sockaddr_in&, const Datagram&)> datagram_tap_;
  std::thread reader_;
  std::atomic<bool> reader_stop_{false};
  std::size_t reader_limit_ = 0;
  mutable std::mutex arrived_mutex_;
  std::vector<Pending> arrived_;
  std::exception_ptr reader_error_;
  std::uint64_t reader_drops_ = 0;
  std::vector<Pending> taken_;
  std::size_t taken_next_ = 0;
};
class QueueLink : public transport::IDatagramLink {
 public:
  QueueLink(UdpSocket& s, sockaddr_in peer) : socket_(s), peer_(peer) {
  }
  void send(const std::uint8_t* p, std::size_t n) override {
    if (socket_.send(peer_, Datagram(p, p + n))) {
      sent_packets_ += 1;
      sent_bytes_ += n;
    }
  }
  bool recv(Datagram& out) override {
    if (inbox.empty())
      return false;
    out = std::move(inbox.front());
    inbox.pop_front();
    return true;
  }
  void poll() override {
  }
  std::deque<Datagram> inbox;
  // Datagrams this link handed to the socket for its peer (cumulative UDP payload bytes).
  std::uint64_t sent_packets() const {
    return sent_packets_;
  }
  std::uint64_t sent_bytes() const {
    return sent_bytes_;
  }

 private:
  UdpSocket& socket_;
  sockaddr_in peer_;
  std::uint64_t sent_packets_ = 0;
  std::uint64_t sent_bytes_ = 0;
};
}  // namespace kcd2s::local
