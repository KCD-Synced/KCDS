// Sequence buffer: a fixed-size ring indexed by a wrapping u16 sequence number, with per-slot
// staleness tags so a wrapped-around old sequence is never mistaken for a live entry. This is the
// standard structure underneath selective ack/nak and reliable-message
// tracking. See also Backend/core/seq.hpp for wrap-aware comparison.
#ifndef KCD2S_TRANSPORT_SEQUENCE_BUFFER_HPP
#define KCD2S_TRANSPORT_SEQUENCE_BUFFER_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include "kcd2s/core/seq.hpp"

namespace kcd2s::transport {

template <class T, std::size_t Size = 1024>
class SequenceBuffer {
  static_assert((Size & (Size - 1)) == 0, "Size must be a power of two");

 public:
  static constexpr std::uint32_t kEmpty = 0xFFFFFFFFu;

  SequenceBuffer() {
    entry_seq_.fill(kEmpty);
  }

  // Insert/replace the entry for `seq`. If `seq` is the newest seen, stale entries in the gap are
  // cleared so they don't masquerade as present after the ring wraps.
  void insert(std::uint16_t seq, const T& value) {
    if (!has_latest_) {
      has_latest_ = true;
      latest_ = seq;
    } else if (core::seq_greater(seq, latest_)) {
      clear_range(static_cast<std::uint16_t>(latest_ + 1), seq);
      latest_ = seq;
    }
    const std::size_t idx = index(seq);
    entry_seq_[idx] = seq;
    entries_[idx] = value;
  }

  T* find(std::uint16_t seq) {
    const std::size_t idx = index(seq);
    return entry_seq_[idx] == seq ? &entries_[idx] : nullptr;
  }
  const T* find(std::uint16_t seq) const {
    const std::size_t idx = index(seq);
    return entry_seq_[idx] == seq ? &entries_[idx] : nullptr;
  }

  bool exists(std::uint16_t seq) const {
    return entry_seq_[index(seq)] == seq;
  }

  void remove(std::uint16_t seq) {
    entry_seq_[index(seq)] = kEmpty;
  }

  bool has_latest() const {
    return has_latest_;
  }
  std::uint16_t latest() const {
    return latest_;
  }

 private:
  static std::size_t index(std::uint16_t seq) {
    return static_cast<std::size_t>(seq) & (Size - 1);
  }

  // Clear [from, to] inclusive, walking forward across the wrap. Bounded to Size steps so a large
  // jump just wipes the whole ring instead of looping forever.
  void clear_range(std::uint16_t from, std::uint16_t to) {
    std::uint32_t span = static_cast<std::uint16_t>(to - from);
    if (span >= Size) {
      entry_seq_.fill(kEmpty);
      return;
    }
    for (std::uint16_t s = from;; ++s) {
      entry_seq_[index(s)] = kEmpty;
      if (s == to)
        break;
    }
  }

  std::array<std::uint32_t, Size> entry_seq_{};
  std::array<T, Size> entries_{};
  std::uint16_t latest_ = 0;
  bool has_latest_ = false;
};

}  // namespace kcd2s::transport

#endif  // KCD2S_TRANSPORT_SEQUENCE_BUFFER_HPP
