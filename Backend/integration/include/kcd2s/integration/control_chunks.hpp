#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

// Control messages longer than one connection message takes. A message on the connection carries
// its length in 16 bits, so one longer than 65535 bytes cannot travel whole (its length wrapped
// and the control channel broke). split_control_message cuts such a message into CONTROL_CHUNKs,
// which the reliable, ordered control channel (0) delivers one after another in order, and
// ControlChunks joins them again on the client, which handles the whole message as if it had
// come in one piece. A shop shelf of thousands of rows (vendor.hpp) or a large inventory needs it.
//
//   CONTROL_CHUNK: [tag][u32 whole length][u32 offset][bytes of the whole message from offset]
namespace kcd2s::integration {

inline constexpr std::uint8_t kControlChunkTag = 179;
// The longest message sent whole, and the bytes of the whole message one chunk carries (a chunk
// fits the connection's 16 KB channel payload).
inline constexpr std::size_t kMaxWholeControlBytes = 60000;
inline constexpr std::size_t kControlChunkBytes = 15000;
// The longest message joined from chunks.
inline constexpr std::size_t kMaxJoinedControlBytes = 4u << 20;

namespace control_chunk_detail {

inline void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(std::uint8_t(value >> shift));
  }
}

inline std::uint32_t get_u32(const std::vector<std::uint8_t>& in, std::size_t at) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= std::uint32_t(in[at + std::size_t(i)]) << (8 * i);
  }
  return value;
}

}  // namespace control_chunk_detail

// The messages that carry `message`: itself when it is short enough, else its chunks in order;
// none for one longer than kMaxJoinedControlBytes.
inline std::vector<std::vector<std::uint8_t>> split_control_message(
    std::vector<std::uint8_t> message) {
  std::vector<std::vector<std::uint8_t>> out;
  if (message.size() <= kMaxWholeControlBytes) {
    out.push_back(std::move(message));
    return out;
  }
  if (message.size() > kMaxJoinedControlBytes) {
    return out;
  }
  for (std::size_t offset = 0; offset < message.size(); offset += kControlChunkBytes) {
    const std::size_t length =
        message.size() - offset < kControlChunkBytes ? message.size() - offset : kControlChunkBytes;
    std::vector<std::uint8_t> chunk;
    chunk.reserve(9 + length);
    chunk.push_back(kControlChunkTag);
    control_chunk_detail::put_u32(chunk, std::uint32_t(message.size()));
    control_chunk_detail::put_u32(chunk, std::uint32_t(offset));
    chunk.insert(chunk.end(), message.begin() + std::ptrdiff_t(offset),
                 message.begin() + std::ptrdiff_t(offset + length));
    out.push_back(std::move(chunk));
  }
  return out;
}

// Joins the chunks of one message at a time, in the order the control channel delivers them.
class ControlChunks {
 public:
  // A CONTROL_CHUNK: the whole message once its last chunk came, else nothing. A chunk that does
  // not follow the one before (another length, a gap) starts over: a message is never joined
  // from pieces of two.
  std::optional<std::vector<std::uint8_t>> take(const std::vector<std::uint8_t>& chunk) {
    using control_chunk_detail::get_u32;
    if (chunk.size() <= 9 || chunk[0] != kControlChunkTag) {
      return std::nullopt;
    }
    const std::uint32_t whole = get_u32(chunk, 1);
    const std::uint32_t offset = get_u32(chunk, 5);
    const std::size_t length = chunk.size() - 9;
    if (whole <= kMaxWholeControlBytes || whole > kMaxJoinedControlBytes ||
        std::size_t(offset) + length > whole) {
      joined_.clear();
      return std::nullopt;
    }
    if (offset == 0) {
      joined_.clear();
      expected_ = whole;
      joined_.reserve(whole);
    } else if (whole != expected_ || offset != joined_.size()) {
      joined_.clear();
      expected_ = 0;
      return std::nullopt;
    }
    joined_.insert(joined_.end(), chunk.begin() + 9, chunk.end());
    if (joined_.size() < expected_) {
      return std::nullopt;
    }
    expected_ = 0;
    return std::exchange(joined_, {});
  }

 private:
  std::vector<std::uint8_t> joined_;
  std::uint32_t expected_ = 0;
};

}  // namespace kcd2s::integration
