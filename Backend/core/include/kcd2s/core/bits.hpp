// KCD2-Synced core — LSB-first bit I/O for the KSTP wire format.
//
// BitWriter packs values into a growable byte buffer, least-significant-bit first within each byte.
// BitReader reads them back in the same order. Partial trailing bits are zero-padded on finish().
// This is the substrate quantization (Backend/core/quant) and the schema codec (Backend/wire/)
// build on.
#ifndef KCD2S_CORE_BITS_HPP
#define KCD2S_CORE_BITS_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace kcd2s::core {

class BitWriter {
 public:
  // Write the low `count` bits of `value` (count in [0, 64]). Higher bits of value are ignored.
  // Fills the current byte, then whole bytes, a byte-sized chunk at a time.
  void write_bits(std::uint64_t value, int count) {
    while (count > 0) {
      if (bit_pos_ == 0)
        bytes_.push_back(0);
      const int space = 8 - bit_pos_;
      const int take = count < space ? count : space;
      const unsigned mask = (1u << take) - 1u;
      const unsigned chunk = static_cast<unsigned>(value) & mask;
      bytes_.back() |= static_cast<std::uint8_t>(chunk << bit_pos_);
      value >>= take;
      count -= take;
      bit_pos_ = (bit_pos_ + take) & 7;
    }
  }

  void write_bool(bool b) {
    write_bits(b ? 1u : 0u, 1);
  }

  // Empty the writer, keeping its buffer for the next use.
  void clear() {
    bytes_.clear();
    bit_pos_ = 0;
    padded_ = false;
  }

  // Write `len` whole bytes at the current bit position, without aligning: the same bits as
  // write_bits(byte, 8) for each byte, in one pass. At an offset p a byte fills the top 8 - p bits
  // of the current byte and starts a new one with its top p bits.
  void write_byte_run(const std::uint8_t* data, std::size_t len) {
    if (len == 0)
      return;
    if (bit_pos_ == 0) {
      bytes_.insert(bytes_.end(), data, data + len);
      return;
    }
    const int shift = bit_pos_;
    const std::size_t partial = bytes_.size() - 1;
    bytes_.resize(bytes_.size() + len);
    std::uint8_t* out = bytes_.data() + partial;
    for (std::size_t i = 0; i < len; ++i) {
      out[i] |= static_cast<std::uint8_t>(data[i] << shift);
      out[i + 1] = static_cast<std::uint8_t>(data[i] >> (8 - shift));
    }
  }

  // LEB128-style variable-length unsigned int: 7 payload bits per group + 1 continuation bit.
  // Cheap for small values (the common case for ids/counts), exact for the full 64-bit range.
  void write_varuint(std::uint64_t v) {
    do {
      std::uint8_t group = static_cast<std::uint8_t>(v & 0x7Fu);
      v >>= 7;
      if (v)
        group |= 0x80u;
      write_bits(group, 8);
    } while (v);
  }

  // Signed varint via zigzag so small-magnitude negatives stay small.
  void write_varint(std::int64_t v) {
    const std::uint64_t zz =
        (static_cast<std::uint64_t>(v) << 1) ^ static_cast<std::uint64_t>(v >> 63);
    write_varuint(zz);
  }

  // Byte-align the write head to the next byte boundary (pads with zero bits).
  void align() {
    if (bit_pos_ != 0) {
      bit_pos_ = 0;  // the current (partial) byte is already in bytes_
      padded_ = true;
    }
  }

  // True once align() inserted padding. Padding depends on the absolute bit position, so such a
  // stream cannot be spliced into another one with append().
  bool padded() const {
    return padded_;
  }

  // Append every bit written to `other`, in order, exactly as if it had been written here. Lets a
  // caller encode an entry once to learn its size and then splice it into the final stream.
  // Returns false (and writes nothing) if `other` contains alignment padding.
  bool append(const BitWriter& other) {
    if (other.padded_)
      return false;
    const std::size_t bits = other.bit_count();
    const std::size_t whole_bytes = bits / 8;
    if (bit_pos_ == 0) {
      // Byte-aligned destination: copy whole bytes directly.
      bytes_.insert(bytes_.end(), other.bytes_.begin(),
                    other.bytes_.begin() + static_cast<std::ptrdiff_t>(whole_bytes));
    } else {
      write_byte_run(other.bytes_.data(), whole_bytes);
    }
    const int rest = static_cast<int>(bits % 8);
    if (rest != 0)
      write_bits(other.bytes_[whole_bytes], rest);
    return true;
  }

  void write_bytes(const std::uint8_t* data, std::size_t len) {
    align();
    bytes_.insert(bytes_.end(), data, data + len);
  }

  // Total bits written so far.
  std::size_t bit_count() const {
    if (bytes_.empty())
      return 0;
    return (bytes_.size() - 1) * 8 + (bit_pos_ == 0 ? 8 : bit_pos_);
  }

  std::size_t byte_size() const {
    return bytes_.size();
  }
  const std::vector<std::uint8_t>& bytes() const {
    return bytes_;
  }

 private:
  std::vector<std::uint8_t> bytes_;
  int bit_pos_ = 0;  // next bit index [0,7] within bytes_.back(); 0 means a fresh byte is needed
  bool padded_ = false;
};

class BitReader {
 public:
  BitReader(const std::uint8_t* data, std::size_t len) : data_(data), len_(len) {
  }
  explicit BitReader(const std::vector<std::uint8_t>& v) : data_(v.data()), len_(v.size()) {
  }

  // Read `count` bits (count in [0,64]) as an LSB-first unsigned value. Out-of-range reads return 0
  // for the missing high bits and set the overrun flag.
  // Reads the rest of the current byte, then whole bytes, a byte-sized chunk at a time.
  std::uint64_t read_bits(int count) {
    std::uint64_t value = 0;
    int done = 0;
    while (done < count) {
      const std::size_t byte_index = bit_cursor_ >> 3;
      if (byte_index >= len_) {
        overrun_ = true;
        break;
      }
      const int bit_in_byte = static_cast<int>(bit_cursor_ & 7);
      const int available = 8 - bit_in_byte;
      const int remaining = count - done;
      const int take = remaining < available ? remaining : available;
      const unsigned mask = (1u << take) - 1u;
      const std::uint64_t chunk = (static_cast<unsigned>(data_[byte_index]) >> bit_in_byte) & mask;
      value |= chunk << done;
      done += take;
      bit_cursor_ += static_cast<std::size_t>(take);
    }
    return value;
  }

  bool read_bool() {
    return read_bits(1) != 0;
  }

  // Read `len` whole bytes from the current bit position: the same as read_bits(8) for each byte,
  // in one pass. A run past the end reads like read_bits (missing bytes 0, overrun set).
  void read_byte_run(std::uint8_t* out, std::size_t len) {
    if (len == 0)
      return;
    if (len > bits_remaining() / 8) {
      for (std::size_t i = 0; i < len; ++i)
        out[i] = static_cast<std::uint8_t>(read_bits(8));
      return;
    }
    const std::size_t first = bit_cursor_ >> 3;
    const int shift = static_cast<int>(bit_cursor_ & 7);
    if (shift == 0) {
      std::memcpy(out, data_ + first, len);
    } else {
      // In range: the last byte read from is first + len, inside the buffer.
      for (std::size_t i = 0; i < len; ++i) {
        const unsigned low = static_cast<unsigned>(data_[first + i]) >> shift;
        const unsigned high = static_cast<unsigned>(data_[first + i + 1]) << (8 - shift);
        out[i] = static_cast<std::uint8_t>(low | high);
      }
    }
    bit_cursor_ += len * 8;
  }

  // Reads a value written by write_varuint. On truncation, sets overrun and returns what was read.
  std::uint64_t read_varuint() {
    std::uint64_t v = 0;
    int shift = 0;
    while (shift < 64) {
      const std::uint64_t group = read_bits(8);
      if (overrun_)
        break;
      v |= (group & 0x7Fu) << shift;
      if ((group & 0x80u) == 0)
        break;
      shift += 7;
    }
    return v;
  }

  std::int64_t read_varint() {
    const std::uint64_t zz = read_varuint();
    return static_cast<std::int64_t>((zz >> 1) ^ (~(zz & 1) + 1));
  }

  void align() {
    bit_cursor_ = (bit_cursor_ + 7) & ~static_cast<std::size_t>(7);
  }

  std::size_t bits_read() const {
    return bit_cursor_;
  }
  bool overrun() const {
    return overrun_;
  }
  std::size_t bits_remaining() const {
    const std::size_t total = len_ * 8;
    return bit_cursor_ >= total ? 0 : total - bit_cursor_;
  }

 private:
  const std::uint8_t* data_;
  std::size_t len_;
  std::size_t bit_cursor_ = 0;
  bool overrun_ = false;
};

}  // namespace kcd2s::core

#endif  // KCD2S_CORE_BITS_HPP
