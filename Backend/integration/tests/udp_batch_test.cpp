// UdpSocket's batched sends over real loopback sockets: between begin_batch and end_batch a
// thread's datagrams wait, then every peer gets exactly its own datagrams, intact and in the order
// they were sent, as one by one; the link counters say what went.
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

// Everything that arrives at `socket` within `wait`, until `count` datagrams came.
std::vector<Datagram> receive(UdpSocket& socket, std::size_t count,
                              std::chrono::milliseconds wait = std::chrono::milliseconds(1000)) {
  std::vector<Datagram> got;
  const auto deadline = std::chrono::steady_clock::now() + wait;
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

// A datagram of `size` bytes that says which peer and which send it was.
Datagram numbered(std::uint8_t peer, std::uint16_t index, std::size_t size) {
  Datagram d(size, std::uint8_t(index * 7 + peer));
  d[0] = peer;
  d[1] = std::uint8_t(index >> 8);
  d[2] = std::uint8_t(index & 0xFF);
  return d;
}

}  // namespace

KCD2S_TEST(udp_batch, every_peer_gets_its_datagrams_in_order) {
  UdpSocket first(address("127.0.0.1", 0));
  UdpSocket second(address("127.0.0.1", 0));
  first.set_buffer_bytes(1 << 22, 1 << 22);
  second.set_buffer_bytes(1 << 22, 1 << 22);
  UdpSocket sender(address("127.0.0.1", 0));
  sender.set_buffer_bytes(1 << 22, 1 << 22);
  const sockaddr_in to_first = address("127.0.0.1", first.local_port());
  const sockaddr_in to_second = address("127.0.0.1", second.local_port());

  // 300 sends to two peers in turn (more than one sendmmsg takes), sizes from 3 to 1203 bytes,
  // and a message's fragments to the first peer every 50th send.
  std::vector<Datagram> wanted_first;
  std::vector<Datagram> wanted_second;
  std::uint64_t first_packets = 0;
  std::uint64_t first_bytes = 0;
  std::uint64_t second_packets = 0;
  std::uint64_t second_bytes = 0;
  sender.begin_batch();
  for (std::uint16_t i = 0; i < 300; ++i) {
    const bool to_one = i % 2 == 0;
    const Datagram d = numbered(to_one ? 1 : 2, i, 3 + (i * 37) % 1201);
    if (to_one) {
      sender.send_counted(to_first, d.data(), d.size(), first_packets, first_bytes);
      wanted_first.push_back(d);
    } else {
      sender.send_counted(to_second, d.data(), d.size(), second_packets, second_bytes);
      wanted_second.push_back(d);
    }
    if (i % 50 == 0) {
      // 4 fragments of 1000 bytes and a last one of 300.
      std::vector<std::uint8_t> message;
      for (std::uint16_t part = 0; part < 5; ++part) {
        const Datagram fragment =
            numbered(1, std::uint16_t(1000 + i + part), part < 4 ? 1000 : 300);
        message.insert(message.end(), fragment.begin(), fragment.end());
        wanted_first.push_back(fragment);
      }
      sender.send_segments_counted(to_first, message.data(), message.size(), 1000, first_packets,
                                   first_bytes);
    }
  }
  sender.end_batch();

  KCD2S_CHECK_EQ(first_packets, std::uint64_t(wanted_first.size()));
  KCD2S_CHECK_EQ(second_packets, std::uint64_t(wanted_second.size()));
  std::uint64_t bytes = 0;
  for (const Datagram& d : wanted_first) {
    bytes += d.size();
  }
  KCD2S_CHECK_EQ(first_bytes, bytes);
  KCD2S_CHECK_EQ(sender.traffic().packets_sent,
                 std::uint64_t(wanted_first.size() + wanted_second.size()));

  const std::vector<Datagram> got_first = receive(first, wanted_first.size());
  const std::vector<Datagram> got_second = receive(second, wanted_second.size());
  KCD2S_CHECK(got_first == wanted_first);
  KCD2S_CHECK(got_second == wanted_second);
}

KCD2S_TEST(udp_batch, a_batch_that_is_not_full_waits_for_its_end) {
  UdpSocket server(address("127.0.0.1", 0));
  UdpSocket sender(address("127.0.0.1", 0));
  const sockaddr_in to = address("127.0.0.1", server.local_port());
  std::uint64_t packets = 0;
  std::uint64_t bytes = 0;
  std::vector<Datagram> wanted;
  sender.begin_batch();
  for (std::uint16_t i = 0; i < 10; ++i) {
    wanted.push_back(numbered(1, i, 100 + i));
    sender.send_counted(to, wanted.back().data(), wanted.back().size(), packets, bytes);
  }
#if defined(__linux__)
  KCD2S_CHECK_EQ(packets, std::uint64_t{0});
  KCD2S_CHECK(receive(server, 1, std::chrono::milliseconds(50)).empty());
#endif
  sender.end_batch();
  KCD2S_CHECK_EQ(packets, std::uint64_t{10});
  KCD2S_CHECK(receive(server, wanted.size()) == wanted);
}

KCD2S_TEST(udp_batch, without_a_batch_every_send_goes_at_once) {
  UdpSocket server(address("127.0.0.1", 0));
  UdpSocket sender(address("127.0.0.1", 0));
  const sockaddr_in to = address("127.0.0.1", server.local_port());
  std::uint64_t packets = 0;
  std::uint64_t bytes = 0;
  const Datagram d = numbered(1, 1, 400);
  sender.send_counted(to, d.data(), d.size(), packets, bytes);
  KCD2S_CHECK_EQ(packets, std::uint64_t{1});
  KCD2S_CHECK_EQ(bytes, std::uint64_t{400});
  const std::vector<Datagram> got = receive(server, 1);
  KCD2S_CHECK(got.size() == 1 && got[0] == d);
}

KCD2S_TEST(udp_batch, another_threads_sends_are_not_held) {
  UdpSocket server(address("127.0.0.1", 0));
  UdpSocket sender(address("127.0.0.1", 0));
  const sockaddr_in to = address("127.0.0.1", server.local_port());
  sender.begin_batch();
  std::uint64_t packets = 0;
  std::uint64_t bytes = 0;
  std::thread other([&] {
    const Datagram d = numbered(3, 3, 200);
    sender.send_counted(to, d.data(), d.size(), packets, bytes);
  });
  other.join();
  KCD2S_CHECK_EQ(packets, std::uint64_t{1});
  KCD2S_CHECK_EQ(receive(server, 1).size(), std::size_t{1});
  sender.end_batch();
}
