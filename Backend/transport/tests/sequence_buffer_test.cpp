#include "kcd2s/transport/sequence_buffer.hpp"

#include <cstdint>

#include "kcd2s/testkit.hpp"

using kcd2s::transport::SequenceBuffer;

KCD2S_TEST(seqbuf, insert_find_remove) {
  SequenceBuffer<int, 256> b;
  KCD2S_CHECK(!b.has_latest());
  b.insert(10, 100);
  b.insert(11, 110);
  KCD2S_CHECK(b.has_latest());
  KCD2S_CHECK_EQ(b.latest(), std::uint16_t{11});
  KCD2S_CHECK(b.exists(10));
  KCD2S_CHECK(b.exists(11));
  KCD2S_CHECK(!b.exists(12));
  KCD2S_CHECK_EQ(*b.find(10), 100);
  b.remove(10);
  KCD2S_CHECK(!b.exists(10));
}

KCD2S_TEST(seqbuf, stale_wrap_entry_not_present) {
  // Size 8: seq and seq+8 map to the same slot. After advancing past the gap, the old one must be
  // reported absent (cleared), not aliased as present.
  SequenceBuffer<int, 8> b;
  b.insert(1, 1);
  KCD2S_CHECK(b.exists(1));
  // Jump far ahead; slot for seq 1 (index 1) will be reused by seq 9.
  b.insert(9, 9);
  KCD2S_CHECK(b.exists(9));
  KCD2S_CHECK(!b.exists(1));  // cleared by the gap walk, not mistaken for present
}

KCD2S_TEST(seqbuf, gap_is_cleared) {
  SequenceBuffer<int, 64> b;
  b.insert(5, 5);
  b.insert(9, 9);  // 6,7,8 were never inserted
  KCD2S_CHECK(!b.exists(6));
  KCD2S_CHECK(!b.exists(7));
  KCD2S_CHECK(!b.exists(8));
  KCD2S_CHECK(b.exists(5));
  KCD2S_CHECK(b.exists(9));
}

KCD2S_TEST(seqbuf, wraparound_latest_tracking) {
  SequenceBuffer<int, 64> b;
  b.insert(65535, 1);
  KCD2S_CHECK_EQ(b.latest(), std::uint16_t{65535});
  b.insert(0, 2);  // wraps; 0 is newer than 65535
  KCD2S_CHECK_EQ(b.latest(), std::uint16_t{0});
  KCD2S_CHECK(b.exists(0));
  KCD2S_CHECK(b.exists(65535));  // adjacent predecessor stays in the window
}

KCD2S_TEST(seqbuf, huge_jump_wipes_ring) {
  SequenceBuffer<int, 16> b;
  for (std::uint16_t s = 0; s < 16; ++s)
    b.insert(s, s);
  KCD2S_CHECK(b.exists(0));
  b.insert(1000, 1000);  // jump >= Size clears everything
  KCD2S_CHECK(b.exists(1000));
  for (std::uint16_t s = 0; s < 16; ++s)
    KCD2S_CHECK(!b.exists(s));
}
