#include "kcd2s/core/bits.hpp"

#include <cstdint>
#include <random>
#include <vector>

#include "kcd2s/core/version.hpp"
#include "kcd2s/testkit.hpp"

using kcd2s::core::BitReader;
using kcd2s::core::BitWriter;

KCD2S_TEST(bits, version_present) {
  KCD2S_CHECK(kcd2s::core::version() != nullptr);
}

KCD2S_TEST(bits, single_bits_roundtrip) {
  BitWriter w;
  w.write_bool(true);
  w.write_bool(false);
  w.write_bool(true);
  w.write_bool(true);
  KCD2S_CHECK_EQ(w.bit_count(), std::size_t{4});

  BitReader r(w.bytes());
  KCD2S_CHECK_EQ(r.read_bool(), true);
  KCD2S_CHECK_EQ(r.read_bool(), false);
  KCD2S_CHECK_EQ(r.read_bool(), true);
  KCD2S_CHECK_EQ(r.read_bool(), true);
  KCD2S_CHECK(!r.overrun());
}

KCD2S_TEST(bits, multi_width_fields) {
  BitWriter w;
  w.write_bits(0x5u, 3);     // 101
  w.write_bits(0x2A3u, 10);  // arbitrary 10-bit value
  w.write_bits(0xFFFFFFFFu, 32);
  w.write_bits(1u, 1);

  BitReader r(w.bytes());
  KCD2S_CHECK_EQ(r.read_bits(3), std::uint64_t{0x5});
  KCD2S_CHECK_EQ(r.read_bits(10), std::uint64_t{0x2A3});
  KCD2S_CHECK_EQ(r.read_bits(32), std::uint64_t{0xFFFFFFFF});
  KCD2S_CHECK_EQ(r.read_bits(1), std::uint64_t{1});
  KCD2S_CHECK(!r.overrun());
}

KCD2S_TEST(bits, high_bits_ignored) {
  BitWriter w;
  w.write_bits(0xFFu, 4);  // only low nibble (0xF) should be written
  BitReader r(w.bytes());
  KCD2S_CHECK_EQ(r.read_bits(4), std::uint64_t{0xF});
}

KCD2S_TEST(bits, full_64_bit_value) {
  const std::uint64_t v = 0x0123456789ABCDEFull;
  BitWriter w;
  w.write_bits(v, 64);
  BitReader r(w.bytes());
  KCD2S_CHECK_EQ(r.read_bits(64), v);
  KCD2S_CHECK(!r.overrun());
}

KCD2S_TEST(bits, overrun_flag_set) {
  BitWriter w;
  w.write_bits(0x3u, 2);  // occupies one byte (buffer is byte-granular)
  BitReader r(w.bytes());
  KCD2S_CHECK_EQ(r.read_bits(8), std::uint64_t{0x3});  // whole byte still readable, no overrun
  KCD2S_CHECK(!r.overrun());
  (void)r.read_bits(1);  // now past the 1-byte buffer
  KCD2S_CHECK(r.overrun());
}

KCD2S_TEST(bits, byte_passthrough_after_align) {
  BitWriter w;
  w.write_bits(0x1u, 1);
  const std::uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
  w.write_bytes(payload, sizeof(payload));

  BitReader r(w.bytes());
  KCD2S_CHECK_EQ(r.read_bits(1), std::uint64_t{1});
  r.align();
  KCD2S_CHECK_EQ(r.read_bits(8), std::uint64_t{0xDE});
  KCD2S_CHECK_EQ(r.read_bits(8), std::uint64_t{0xAD});
  KCD2S_CHECK_EQ(r.read_bits(8), std::uint64_t{0xBE});
  KCD2S_CHECK_EQ(r.read_bits(8), std::uint64_t{0xEF});
}

KCD2S_TEST(bits, fuzz_random_fields_roundtrip) {
  std::mt19937_64 rng(0xC0FFEEu);
  for (int iter = 0; iter < 500; ++iter) {
    struct Field {
      std::uint64_t value;
      int width;
    };
    std::vector<Field> fields;
    BitWriter w;
    std::uniform_int_distribution<int> count_dist(1, 40);
    const int n = count_dist(rng);
    for (int i = 0; i < n; ++i) {
      std::uniform_int_distribution<int> width_dist(1, 64);
      const int width = width_dist(rng);
      const std::uint64_t raw = rng();
      const std::uint64_t mask = (width == 64) ? ~0ull : ((1ull << width) - 1);
      const std::uint64_t value = raw & mask;
      fields.push_back({value, width});
      w.write_bits(value, width);
    }
    BitReader r(w.bytes());
    for (const auto& f : fields)
      KCD2S_CHECK_EQ(r.read_bits(f.width), f.value);
    KCD2S_CHECK(!r.overrun());
  }
}

// budgeted snapshots encode an entry once and splice it into the final stream.
KCD2S_TEST(bits, append_splices_unaligned_streams_exactly) {
  std::mt19937 rng(1234);
  BitWriter direct;
  BitWriter spliced;
  for (int entry = 0; entry < 50; ++entry) {
    BitWriter piece;
    const int fields = 1 + static_cast<int>(rng() % 6);
    for (int f = 0; f < fields; ++f) {
      const int width = 1 + static_cast<int>(rng() % 40);
      const std::uint64_t value = rng();
      direct.write_bits(value, width);
      piece.write_bits(value, width);
    }
    direct.write_varuint(static_cast<std::uint64_t>(entry) * 977u);
    piece.write_varuint(static_cast<std::uint64_t>(entry) * 977u);
    KCD2S_CHECK(spliced.append(piece));
  }
  KCD2S_CHECK_EQ(spliced.bit_count(), direct.bit_count());
  KCD2S_CHECK(spliced.bytes() == direct.bytes());
}

KCD2S_TEST(bits, append_refuses_padded_streams) {
  BitWriter padded;
  padded.write_bits(1, 3);
  padded.align();
  padded.write_bits(5, 8);
  KCD2S_CHECK(padded.padded());
  BitWriter target;
  target.write_bits(1, 1);
  KCD2S_CHECK(!target.append(padded));
  KCD2S_CHECK_EQ(target.bit_count(), std::size_t{1});  // nothing written
  BitWriter aligned_already;
  aligned_already.align();  // no partial byte: no padding
  KCD2S_CHECK(!aligned_already.padded());
}

namespace {

// Bit-at-a-time reference for BitReader::read_bits, including its overrun behavior.
std::uint64_t reference_read(const std::vector<std::uint8_t>& data, std::size_t& cursor, int count,
                             bool& overrun) {
  std::uint64_t value = 0;
  for (int i = 0; i < count; ++i) {
    const std::size_t byte_index = cursor >> 3;
    if (byte_index >= data.size()) {
      overrun = true;
      break;
    }
    const std::uint64_t bit = (data[byte_index] >> (cursor & 7)) & 1u;
    value |= bit << i;
    ++cursor;
  }
  return value;
}

}  // namespace

KCD2S_TEST(bits, chunked_reads_match_bit_by_bit_reads) {
  std::mt19937 rng(98765);
  std::vector<std::uint8_t> data(257);
  for (auto& byte : data)
    byte = static_cast<std::uint8_t>(rng());
  BitReader reader(data);
  std::size_t cursor = 0;
  bool overrun = false;
  bool identical = true;
  // Random widths until well past the end, so the overrun path is compared too.
  for (int i = 0; i < 200; ++i) {
    const int width = static_cast<int>(rng() % 65);
    const std::uint64_t expected = reference_read(data, cursor, width, overrun);
    const std::uint64_t actual = reader.read_bits(width);
    if (expected != actual || reader.overrun() != overrun)
      identical = false;
  }
  KCD2S_CHECK(identical);
  KCD2S_CHECK(reader.overrun());
}

KCD2S_TEST(bits, byte_runs_match_bytewise_bits_at_every_offset) {
  std::mt19937 rng(7);
  std::vector<std::uint8_t> payload(37);
  for (auto& byte : payload)
    byte = static_cast<std::uint8_t>(rng());
  bool same_written = true;
  bool same_read = true;
  for (int offset = 0; offset < 8; ++offset) {
    BitWriter bytewise;
    BitWriter run;
    bytewise.write_bits(0x55u, offset);
    run.write_bits(0x55u, offset);
    for (std::uint8_t byte : payload)
      bytewise.write_bits(byte, 8);
    run.write_byte_run(payload.data(), payload.size());
    bytewise.write_bits(0x3u, 3);
    run.write_bits(0x3u, 3);
    if (bytewise.bytes() != run.bytes() || bytewise.bit_count() != run.bit_count())
      same_written = false;

    // Read back at the same offset, then one byte past the end (zero and overrun, like read_bits).
    BitReader reference(bytewise.bytes());
    BitReader reader(bytewise.bytes());
    reference.read_bits(offset);
    reader.read_bits(offset);
    std::vector<std::uint8_t> expected(payload.size());
    for (auto& byte : expected)
      byte = static_cast<std::uint8_t>(reference.read_bits(8));
    std::vector<std::uint8_t> got(payload.size());
    reader.read_byte_run(got.data(), got.size());
    if (got != expected || got != payload || reader.bits_read() != reference.bits_read())
      same_read = false;
    std::vector<std::uint8_t> past(2, 0xAA);
    reader.read_byte_run(past.data(), past.size());
    const std::uint64_t first = reference.read_bits(8);
    const std::uint64_t second = reference.read_bits(8);
    if (past[0] != first || past[1] != second || !reader.overrun() || !reference.overrun())
      same_read = false;
  }
  KCD2S_CHECK(same_written);
  KCD2S_CHECK(same_read);
}
