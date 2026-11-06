#include <cstdint>
#include <random>

#include "kcd2s/core/bits.hpp"
#include "kcd2s/core/seq.hpp"
#include "kcd2s/testkit.hpp"

using kcd2s::core::BitReader;
using kcd2s::core::BitWriter;

KCD2S_TEST(varint, unsigned_boundaries) {
  const std::uint64_t values[] = {0u,
                                  1u,
                                  127u,
                                  128u,
                                  255u,
                                  16383u,
                                  16384u,
                                  0xFFFFFFFFu,
                                  0x1'0000'0000ull,
                                  0xFFFF'FFFF'FFFF'FFFFull};
  for (std::uint64_t v : values) {
    BitWriter w;
    w.write_varuint(v);
    BitReader r(w.bytes());
    KCD2S_CHECK_EQ(r.read_varuint(), v);
    KCD2S_CHECK(!r.overrun());
  }
}

KCD2S_TEST(varint, small_values_are_compact) {
  BitWriter w;
  w.write_varuint(5);  // single group
  KCD2S_CHECK_EQ(w.byte_size(), std::size_t{1});
}

KCD2S_TEST(varint, signed_zigzag_roundtrip) {
  const std::int64_t values[] = {0, -1, 1, -2, 2, 63, -64, 1000, -1000, INT64_MAX, INT64_MIN};
  for (std::int64_t v : values) {
    BitWriter w;
    w.write_varint(v);
    BitReader r(w.bytes());
    KCD2S_CHECK_EQ(r.read_varint(), v);
  }
}

KCD2S_TEST(varint, signed_small_negatives_are_compact) {
  BitWriter w;
  w.write_varint(-1);  // zigzag -> 1 -> single group
  KCD2S_CHECK_EQ(w.byte_size(), std::size_t{1});
}

KCD2S_TEST(varint, fuzz_mixed_stream) {
  std::mt19937_64 rng(0xBEEF);
  for (int iter = 0; iter < 300; ++iter) {
    std::vector<std::uint64_t> us;
    BitWriter w;
    const int n = static_cast<int>(rng() % 20) + 1;
    for (int i = 0; i < n; ++i) {
      const std::uint64_t v = rng();
      us.push_back(v);
      w.write_varuint(v);
    }
    BitReader r(w.bytes());
    for (std::uint64_t expected : us)
      KCD2S_CHECK_EQ(r.read_varuint(), expected);
    KCD2S_CHECK(!r.overrun());
  }
}

KCD2S_TEST(seq, u16_wraparound) {
  using kcd2s::core::seq_greater;
  KCD2S_CHECK(seq_greater(std::uint16_t{2}, std::uint16_t{1}));
  KCD2S_CHECK(!seq_greater(std::uint16_t{1}, std::uint16_t{2}));
  // 0 is "greater" than 65535 across the wrap.
  KCD2S_CHECK(seq_greater(std::uint16_t{0}, std::uint16_t{65535}));
  KCD2S_CHECK(!seq_greater(std::uint16_t{65535}, std::uint16_t{0}));
}

KCD2S_TEST(seq, u32_wraparound_and_equal) {
  using kcd2s::core::seq_greater;
  using kcd2s::core::seq_greater_equal;
  KCD2S_CHECK(seq_greater(std::uint32_t{0}, std::uint32_t{0xFFFFFFFFu}));
  KCD2S_CHECK(!seq_greater(std::uint32_t{5}, std::uint32_t{5}));
  KCD2S_CHECK(seq_greater_equal(std::uint32_t{5}, std::uint32_t{5}));
}
