// Sequence-number comparison across wraparound.
// seqGreater(a,b) = signed difference > 0, so comparisons stay correct as counters wrap at 2^N.
#ifndef KCD2S_CORE_SEQ_HPP
#define KCD2S_CORE_SEQ_HPP

#include <cstdint>

namespace kcd2s::core {

inline bool seq_greater(std::uint16_t a, std::uint16_t b) {
  return static_cast<std::int16_t>(a - b) > 0;
}
inline bool seq_greater(std::uint32_t a, std::uint32_t b) {
  return static_cast<std::int32_t>(a - b) > 0;
}

inline bool seq_greater_equal(std::uint16_t a, std::uint16_t b) {
  return static_cast<std::int16_t>(a - b) >= 0;
}
inline bool seq_greater_equal(std::uint32_t a, std::uint32_t b) {
  return static_cast<std::int32_t>(a - b) >= 0;
}

}  // namespace kcd2s::core

#endif  // KCD2S_CORE_SEQ_HPP
