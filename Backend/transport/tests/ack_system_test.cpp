#include "kcd2s/transport/ack_system.hpp"

#include <algorithm>
#include <cstdint>

#include "kcd2s/testkit.hpp"

using kcd2s::transport::AckSystem;

static bool contains(const std::vector<std::uint16_t>& v, std::uint16_t x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}

KCD2S_TEST(ack, sequence_allocation_wraps) {
  AckSystem a;
  KCD2S_CHECK_EQ(a.next_sequence(), std::uint16_t{0});
  KCD2S_CHECK_EQ(a.next_sequence(), std::uint16_t{1});
  AckSystem b;
  for (int i = 0; i < 65535; ++i)
    (void)b.next_sequence();
  KCD2S_CHECK_EQ(b.next_sequence(), std::uint16_t{65535});
  KCD2S_CHECK_EQ(b.next_sequence(), std::uint16_t{0});  // wrap
}

KCD2S_TEST(ack, cold_before_any_receive) {
  AckSystem a;
  KCD2S_CHECK(!a.has_received());
  std::uint16_t ack_seq = 7;
  std::uint32_t ack_bits = 7;
  a.generate_ack(ack_seq, ack_bits);
  KCD2S_CHECK_EQ(ack_seq, std::uint16_t{0});
  KCD2S_CHECK_EQ(ack_bits, std::uint32_t{0});
}

KCD2S_TEST(ack, ackbits_reflect_received_window) {
  AckSystem a;
  a.on_packet_received(100);
  a.on_packet_received(99);
  a.on_packet_received(97);  // 98 deliberately missing

  std::uint16_t ack_seq = 0;
  std::uint32_t ack_bits = 0;
  a.generate_ack(ack_seq, ack_bits);
  KCD2S_CHECK_EQ(ack_seq, std::uint16_t{100});
  // bit0 => seq 99 present, bit1 => seq 98 missing, bit2 => seq 97 present
  KCD2S_CHECK((ack_bits & (1u << 0)) != 0);
  KCD2S_CHECK((ack_bits & (1u << 1)) == 0);
  KCD2S_CHECK((ack_bits & (1u << 2)) != 0);
}

KCD2S_TEST(ack, process_ack_marks_newly_acked_once) {
  AckSystem a;
  // Pretend we sent seqs 0..3 at t=0.
  for (std::uint16_t s = 0; s < 4; ++s)
    a.on_packet_sent(s, 0.0);

  // Peer acks seq 3 with bits covering 2,1,0.
  const std::uint32_t bits = (1u << 0) | (1u << 1) | (1u << 2);  // 2,1,0
  auto first = a.on_ack_received(3, bits, 0.05);
  KCD2S_CHECK_EQ(first.size(), std::size_t{4});
  KCD2S_CHECK(contains(first, 0) && contains(first, 1) && contains(first, 2) && contains(first, 3));

  // Re-processing the same ack reports nothing new.
  auto again = a.on_ack_received(3, bits, 0.05);
  KCD2S_CHECK_EQ(again.size(), std::size_t{0});
}

KCD2S_TEST(ack, rtt_estimated_from_send_times) {
  AckSystem a;
  a.on_packet_sent(0, 1.0);
  auto acked = a.on_ack_received(0, 0, 1.2);  // 200ms round trip
  KCD2S_CHECK_EQ(acked.size(), std::size_t{1});
  const double rtt = a.rtt_seconds();
  KCD2S_CHECK(rtt > 0.19 && rtt < 0.21);
}

KCD2S_TEST(ack, unsent_or_lost_seqs_not_acked) {
  AckSystem a;
  a.on_packet_sent(5, 0.0);
  // Ack claims seq 5 plus a preceding seq 4 we never sent; only 5 should be reported.
  auto acked = a.on_ack_received(5, (1u << 0), 0.01);
  KCD2S_CHECK_EQ(acked.size(), std::size_t{1});
  KCD2S_CHECK(contains(acked, 5));
}

KCD2S_TEST(ack, roundtrip_two_systems) {
  // Two peers exchanging acks: A sends, B receives and acks, A learns delivery.
  AckSystem A, B;
  const std::uint16_t s0 = A.next_sequence();
  const std::uint16_t s1 = A.next_sequence();
  A.on_packet_sent(s0, 0.0);
  A.on_packet_sent(s1, 0.0);
  B.on_packet_received(s0);
  B.on_packet_received(s1);

  std::uint16_t ack_seq = 0;
  std::uint32_t ack_bits = 0;
  B.generate_ack(ack_seq, ack_bits);
  auto acked = A.on_ack_received(ack_seq, ack_bits, 0.03);
  KCD2S_CHECK(contains(acked, s0));
  KCD2S_CHECK(contains(acked, s1));
}
