// UdpSocket::send_segments over real loopback sockets: the datagrams of one call arrive as the
// datagrams they were, each on its own and intact, with UDP segmentation offload where the
// kernel has it and one by one where it does not.
#include "../local/udp_posix.hpp"

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "kcd2s/testkit.hpp"

using kcd2s::local::address;
using kcd2s::local::Datagram;
using kcd2s::local::UdpSocket;

namespace {

// Everything that arrives at `socket` within a second, until `count` datagrams came.
std::vector<Datagram> receive(UdpSocket& socket, std::size_t count) {
  std::vector<Datagram> got;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (got.size() < count && std::chrono::steady_clock::now() < deadline) {
    sockaddr_in from{};
    Datagram d;
    if (socket.recv(from, d)) {
      got.push_back(std::move(d));
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  return got;
}

}  // namespace

KCD2S_TEST(udp_segments, every_datagram_arrives_on_its_own) {
  UdpSocket server(address("127.0.0.1", 0));
  server.set_buffer_bytes(1 << 20, 1 << 20);
  UdpSocket sender(address("127.0.0.1", 0));
  const sockaddr_in to = address("127.0.0.1", server.local_port());
  // 150 datagrams of 1200 bytes and a last one of 500: more than one offloaded call takes.
  constexpr std::size_t kSegment = 1200;
  constexpr std::size_t kFull = 150;
  std::vector<std::uint8_t> bytes;
  for (std::size_t i = 0; i < kFull; ++i) {
    bytes.insert(bytes.end(), kSegment, std::uint8_t(i));
  }
  bytes.insert(bytes.end(), 500, std::uint8_t(0xEE));
  const UdpSocket::SegmentsSent sent =
      sender.send_segments(to, bytes.data(), bytes.size(), kSegment);
  KCD2S_CHECK_EQ(sent.packets, std::uint64_t{kFull + 1});
  KCD2S_CHECK_EQ(sent.bytes, std::uint64_t(bytes.size()));
  KCD2S_CHECK_EQ(sender.traffic().packets_sent, std::uint64_t{kFull + 1});

  const std::vector<Datagram> got = receive(server, kFull + 1);
  KCD2S_CHECK_EQ(got.size(), kFull + 1);
  bool intact = got.size() == kFull + 1;
  for (std::size_t i = 0; intact && i < kFull; ++i) {
    intact = got[i] == Datagram(kSegment, std::uint8_t(i));
  }
  KCD2S_CHECK(intact);
  KCD2S_CHECK(got.size() == kFull + 1 && got.back() == Datagram(500, std::uint8_t(0xEE)));
}

KCD2S_TEST(udp_segments, a_single_datagram_goes_as_one) {
  UdpSocket server(address("127.0.0.1", 0));
  UdpSocket sender(address("127.0.0.1", 0));
  const sockaddr_in to = address("127.0.0.1", server.local_port());
  const std::vector<std::uint8_t> bytes(700, 7);
  const UdpSocket::SegmentsSent sent = sender.send_segments(to, bytes.data(), bytes.size(), 1200);
  KCD2S_CHECK_EQ(sent.packets, std::uint64_t{1});
  const std::vector<Datagram> got = receive(server, 1);
  KCD2S_CHECK(got.size() == 1 && got[0] == Datagram(bytes));
}
