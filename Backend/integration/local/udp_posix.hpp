#pragma once
// The server's UDP socket on Linux and other POSIX systems: the same interface as
// udp_win.hpp, on BSD sockets.
#ifdef _WIN32
#error "udp_posix.hpp is for POSIX systems; Windows uses udp_win.hpp"
#endif

#include <functional>
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
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

// Sockets need no process-wide start-up on POSIX; kept so the callers read the same everywhere.
struct Winsock {
  Winsock() {
  }
};

inline sockaddr_in address(const char* ip, unsigned short port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) {
    throw std::runtime_error("Expected numeric IPv4 address");
  }
  return a;
}

inline std::uint64_t key(const sockaddr_in& a) {
  return (std::uint64_t(a.sin_addr.s_addr) << 16) | a.sin_port;
}

class UdpSocket {
 public:
  explicit UdpSocket(const sockaddr_in& local) {
    // Close-on-exec: the resource host child must not inherit the server's socket.
    fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
    if (fd_ < 0) {
      throw std::runtime_error("socket failed");
    }
    const int flags = fcntl(fd_, F_GETFL, 0);
    if (bind(fd_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0 || flags < 0 ||
        fcntl(fd_, F_SETFL, flags | O_NONBLOCK) != 0) {
      close(fd_);
      fd_ = -1;
      throw std::runtime_error("UDP bind/nonblocking setup failed");
    }
  }
  ~UdpSocket() {
    if (reader_.joinable()) {
      reader_stop_.store(true, std::memory_order_relaxed);
      reader_.join();
    }
    if (fd_ >= 0) {
      close(fd_);
    }
  }
  // Read the socket on a thread of its own from now on (see udp_win.hpp): it takes every
  // datagram off the OS buffer as it arrives and keeps up to `limit` of them (more are dropped
  // and counted); recv() hands them out on the caller's thread. A receive error of the thread is
  // thrown by recv().
  // A datagram this says is taken (voice) goes no further. Set before start_reader.
  void set_datagram_tap(std::function<bool(const sockaddr_in&, const Datagram&)> tap) {
    datagram_tap_ = std::move(tap);
  }
  void start_reader(std::size_t limit) {
    if (reader_.joinable()) {
      return;
    }
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
  // once per tick needs room for a whole tick of input. Linux caps the request at
  // net.core.rmem_max / wmem_max. Returns false if the OS refused.
  bool set_buffer_bytes(int receive, int send) {
    const bool receive_ok = setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &receive, sizeof(receive)) == 0;
    const bool send_ok = setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &send, sizeof(send)) == 0;
    return receive_ok && send_ok;
  }
  // Buffer sizes the OS reports for this socket (0 if the query fails).
  int receive_buffer_bytes() const {
    return socket_option(SO_RCVBUF);
  }
  int send_buffer_bytes() const {
    return socket_option(SO_SNDBUF);
  }

  // Measurement tools only: emulate random loss and a fixed one-way delay in both directions of
  // this socket (see udp_win.hpp).
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
    return send(to, d.data(), d.size());
  }
  bool send(const sockaddr_in& to, const std::uint8_t* data, std::size_t len) {
    if (len > 65507) {
      throw std::runtime_error("UDP payload too large");
    }
    if (outbound_) {
      std::lock_guard<std::mutex> lock(outbound_mutex_);
      outbound_->offer(impairment_clock(), Pending{to, Datagram(data, data + len)});
      flush_outbound();
      return true;
    }
    return send_now(to, data, len);
  }
  // What the system accepted of a send_segments call.
  struct SegmentsSent {
    std::uint64_t packets = 0;
    std::uint64_t bytes = 0;
  };
  // `len` bytes of datagrams of `segment` bytes each to one peer, back to back (the last may be
  // shorter): the fragments of one message. With UDP segmentation offload (Linux UDP_SEGMENT)
  // they go down in one call and pass the network stack once instead of once each; where the
  // kernel or the network device cannot, every datagram is sent on its own, as send() does.
  SegmentsSent send_segments(const sockaddr_in& to, const std::uint8_t* data, std::size_t len,
                             std::size_t segment) {
    SegmentsSent sent;
    std::size_t off = 0;
#if defined(__linux__)
    const std::size_t per_call = std::min(kMaxOffloadSegments, kMaxOffloadBytes / segment);
    while (!outbound_ && segment_offload_.load(std::memory_order_relaxed) && per_call >= 2 &&
           len - off > segment) {
      const std::size_t n = std::min(len - off, per_call * segment);
      if (!send_offloaded(to, data + off, n, segment, sent)) {
        break;
      }
      off += n;
    }
#endif
    for (; off < len; off += segment) {
      const std::size_t n = std::min(segment, len - off);
      if (send(to, data + off, n)) {
        sent.packets += 1;
        sent.bytes += n;
      }
    }
    return sent;
  }
  bool recv(sockaddr_in& from, Datagram& out) {
    if (!inbound_) {
      if (!next_arrived(from, out)) {
        return false;
      }
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
    if (!inbound_->take_due(now, due)) {
      return false;
    }
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
    if (fd_ < 0 || d.empty() || d.size() > 65507) {
      return false;
    }
    const ssize_t n =
        sendto(fd_, d.data(), d.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    return n == static_cast<ssize_t>(d.size());
  }
  // The bound local port (useful after binding port 0 for an ephemeral port).
  unsigned short local_port() const {
    sockaddr_in local{};
    socklen_t len = sizeof(local);
    if (getsockname(fd_, reinterpret_cast<sockaddr*>(&local), &len) != 0) {
      throw std::runtime_error("getsockname failed");
    }
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
  // A send error a moment of network trouble causes: the datagram is lost, like any other, and the
  // server goes on (udp_win.hpp has the same list).
  static bool passing_send_error(int code) {
    switch (code) {
      case ENOBUFS:
      case ENETDOWN:
      case ENETUNREACH:
      case EHOSTUNREACH:
      case ENETRESET:
      case ECONNREFUSED:
      case ECONNRESET:
      case EADDRNOTAVAIL:
      case EPERM:
      case EINTR:
        return true;
      default:
        return false;
    }
  }
#if defined(__linux__)
  // The UDP_SEGMENT socket option (linux/udp.h; not every libc names it).
  static constexpr int kUdpSegment = 103;
  // One UDP_SEGMENT call carries at most this many datagrams and bytes (the kernel's limits:
  // 64 segments on older kernels, and one IP packet's 64 KiB).
  static constexpr std::size_t kMaxOffloadSegments = 64;
  static constexpr std::size_t kMaxOffloadBytes = 65000;
  // Sends `len` bytes as datagrams of `segment` bytes in one sendmsg with UDP_SEGMENT. False
  // when this socket cannot (an old kernel, a device without checksum offload): offload is then
  // off for good and the caller sends the datagrams one by one.
  bool send_offloaded(const sockaddr_in& to, const std::uint8_t* data, std::size_t len,
                      std::size_t segment, SegmentsSent& sent) {
    iovec part{const_cast<std::uint8_t*>(data), len};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(std::uint16_t))] = {};
    msghdr message{};
    message.msg_name = const_cast<sockaddr_in*>(&to);
    message.msg_namelen = sizeof(to);
    message.msg_iov = &part;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    cmsghdr* option = CMSG_FIRSTHDR(&message);
    option->cmsg_level = IPPROTO_UDP;
    option->cmsg_type = kUdpSegment;
    option->cmsg_len = CMSG_LEN(sizeof(std::uint16_t));
    const auto size = static_cast<std::uint16_t>(segment);
    std::memcpy(CMSG_DATA(option), &size, sizeof(size));
    const std::size_t packets = (len + segment - 1) / segment;
    if (sendmsg(fd_, &message, 0) < 0) {
      const int code = errno;
      if (code == EIO || code == EINVAL || code == ENOPROTOOPT || code == EOPNOTSUPP) {
        segment_offload_.store(false, std::memory_order_relaxed);
        return false;
      }
      if (passing_send_error(code) || code == EAGAIN || code == EWOULDBLOCK) {
        std::lock_guard<std::mutex> lock(traffic_mutex_);
        for (std::size_t i = 0; i < packets; ++i) {
          if (code == EAGAIN || code == EWOULDBLOCK) {
            traffic_.record_would_block();
          } else {
            traffic_.record_dropped();
          }
        }
        return true;
      }
      throw std::runtime_error("UDP send failed (errno " + std::to_string(code) + ")");
    }
    std::lock_guard<std::mutex> lock(traffic_mutex_);
    for (std::size_t off = 0; off < len; off += segment) {
      traffic_.record_sent(std::min(segment, len - off));
    }
    sent.packets += packets;
    sent.bytes += len;
    return true;
  }
#endif
  bool send_now(const sockaddr_in& to, const Datagram& d) {
    return send_now(to, d.data(), d.size());
  }
  bool send_now(const sockaddr_in& to, const std::uint8_t* data, std::size_t len) {
    const ssize_t n = sendto(fd_, data, len, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    if (n < 0) {
      const int code = errno;
      if (passing_send_error(code)) {
        std::lock_guard<std::mutex> lock(traffic_mutex_);
        traffic_.record_dropped();
        return false;
      }
      if (code != EAGAIN && code != EWOULDBLOCK) {
        throw std::runtime_error("UDP send failed (errno " + std::to_string(code) + ")");
      }
      std::lock_guard<std::mutex> lock(traffic_mutex_);
      traffic_.record_would_block();
      return false;
    }
    std::lock_guard<std::mutex> lock(traffic_mutex_);
    traffic_.record_sent(len);
    return true;
  }
  // A failed earlier send can leave an ICMP error on a UDP socket (ECONNREFUSED); like
  // WSAECONNRESET on Windows it ends this drain and the next call reads on.
  bool receive_now(sockaddr_in& from, Datagram& out) {
    std::uint8_t bytes[65536];
    socklen_t len = sizeof(from);
    const ssize_t n =
        recvfrom(fd_, bytes, sizeof(bytes), 0, reinterpret_cast<sockaddr*>(&from), &len);
    if (n < 0) {
      const int e = errno;
      if (e == EAGAIN || e == EWOULDBLOCK || e == ECONNREFUSED || e == EINTR) {
        return false;
      }
      throw std::runtime_error("UDP recv failed");
    }
    out.assign(bytes, bytes + n);
    return true;
  }
  // The next datagram: from the reader when it runs, otherwise straight from the socket.
  bool next_arrived(sockaddr_in& from, Datagram& out) {
    if (!reader_.joinable()) {
      return receive_now(from, out);
    }
    if (taken_next_ == taken_.size()) {
      taken_.clear();
      taken_next_ = 0;
      std::lock_guard<std::mutex> lock(arrived_mutex_);
      if (reader_error_) {
        std::rethrow_exception(reader_error_);
      }
      taken_.swap(arrived_);
      if (taken_.empty()) {
        return false;
      }
    }
    Pending& next = taken_[taken_next_++];
    from = next.address;
    out = std::move(next.data);
    return true;
  }
  void read_loop() {
    pollfd readable{fd_, POLLIN, 0};
    std::vector<Pending> batch;
    while (!reader_stop_.load(std::memory_order_relaxed)) {
      readable.revents = 0;
      const int ready = poll(&readable, 1, 20);  // wakes up to see a stop request
      if (ready < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      if (ready == 0) {
        continue;
      }
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
      if (batch.empty()) {
        continue;
      }
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
    socklen_t length = sizeof(value);
    if (getsockopt(fd_, SOL_SOCKET, name, &value, &length) != 0) {
      return 0;
    }
    return value;
  }

  int fd_ = -1;
  // Sends come from several threads at once (Server::tick flushes its clients in parallel): the
  // traffic counters and the outbound impairment queue are shared. The queue's lock is taken
  // first when both are held. Receives and traffic() run on the host thread between ticks.
  std::mutex traffic_mutex_;
  std::mutex outbound_mutex_;
  metrics::TrafficCounters traffic_;
  std::unique_ptr<netsim::ImpairedQueue<Pending>> outbound_;
  std::unique_ptr<netsim::ImpairedQueue<Pending>> inbound_;
  // The reader thread (start_reader) and what it hands to recv().
  std::function<bool(const sockaddr_in&, const Datagram&)> datagram_tap_;
  std::thread reader_;
  std::atomic<bool> reader_stop_{false};
  // Fragments go down with UDP segmentation offload until the socket refuses it once.
  std::atomic<bool> segment_offload_{true};
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
    if (socket_.send(peer_, p, n)) {
      sent_packets_ += 1;
      sent_bytes_ += n;
    }
  }
  void send_segments(const std::uint8_t* p, std::size_t n, std::size_t segment) override {
    const UdpSocket::SegmentsSent sent = socket_.send_segments(peer_, p, n, segment);
    sent_packets_ += sent.packets;
    sent_bytes_ += sent.bytes;
  }
  bool recv(Datagram& out) override {
    if (inbox.empty()) {
      return false;
    }
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
