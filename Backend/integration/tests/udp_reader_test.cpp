// The server's socket reader thread (UdpSocket::start_reader): datagrams sent to the socket come
// out of recv() on the caller's thread, in order and intact, and a socket closes with its reader
// running.
#include "../local/udp_win.hpp"

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "kcd2s/testkit.hpp"

using kcd2s::local::address;
using kcd2s::local::Datagram;
using kcd2s::local::UdpSocket;

namespace {

const kcd2s::local::Winsock& winsock() {
  static const kcd2s::local::Winsock instance;
  return instance;
}

}  // namespace

KCD2S_TEST(udp_reader, datagrams_arrive_in_order_and_intact) {
  winsock();
  UdpSocket server(address("127.0.0.1", 0));
  server.set_buffer_bytes(1 << 20, 1 << 20);
  server.start_reader(1024);
  UdpSocket sender(address("127.0.0.1", 0));
  const sockaddr_in to = address("127.0.0.1", server.local_port());
  constexpr int kCount = 300;
  for (int i = 0; i < kCount; ++i) {
    Datagram d(std::size_t(1 + i % 700), std::uint8_t(i));
    d[0] = std::uint8_t(i & 0xff);
    sender.send(to, d);
  }
  std::vector<Datagram> got;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (int(got.size()) < kCount && std::chrono::steady_clock::now() < deadline) {
    sockaddr_in from{};
    Datagram d;
    if (server.recv(from, d)) {
      KCD2S_CHECK_EQ(from.sin_port, htons(sender.local_port()));
      got.push_back(std::move(d));
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  KCD2S_CHECK_EQ(int(got.size()), kCount);
  bool intact = true;
  for (int i = 0; i < int(got.size()); ++i) {
    if (got[i].size() != std::size_t(1 + i % 700) || got[i][0] != std::uint8_t(i & 0xff))
      intact = false;
  }
  KCD2S_CHECK(intact);
  KCD2S_CHECK_EQ(server.reader_drops(), std::uint64_t{0});
  KCD2S_CHECK_EQ(server.traffic().packets_received, std::uint64_t(kCount));
}

KCD2S_TEST(udp_reader, a_full_queue_drops_and_counts) {
  winsock();
  UdpSocket server(address("127.0.0.1", 0));
  server.set_buffer_bytes(1 << 20, 1 << 20);
  server.start_reader(10);
  UdpSocket sender(address("127.0.0.1", 0));
  const sockaddr_in to = address("127.0.0.1", server.local_port());
  for (int i = 0; i < 50; ++i)
    sender.send(to, Datagram(32, std::uint8_t(i)));
  // Nobody calls recv: the reader keeps 10 and drops the rest.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (server.reader_drops() < 40 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  KCD2S_CHECK_EQ(server.reader_drops(), std::uint64_t{40});
  int kept = 0;
  sockaddr_in from{};
  Datagram d;
  while (server.recv(from, d))
    ++kept;
  KCD2S_CHECK_EQ(kept, 10);
}

KCD2S_TEST(udp_reader, closes_with_the_reader_running) {
  winsock();
  for (int i = 0; i < 5; ++i) {
    UdpSocket server(address("127.0.0.1", 0));
    server.start_reader(16);
  }
  KCD2S_CHECK(true);
}
