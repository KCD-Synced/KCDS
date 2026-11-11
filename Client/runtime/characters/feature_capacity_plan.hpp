// Bounded startup allocation policy for the native clothing material feature buffer.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace kcds::runtime::characters::features {

// Each clothing material that has colour features takes one block of 27 features (its dyes and
// patterns). The native buffer holds 2048 blocks; a crowd of hundreds of dressed bodies uses them
// all up, and every material after that draws with no features: white cloth.
inline constexpr unsigned kDefaultScale = 8;
inline constexpr unsigned kMaximumScale = 16;
inline constexpr std::uint32_t kBlockFeatures = 27;
inline constexpr std::uint32_t kNativeFeatures = 2048 * kBlockFeatures;
inline constexpr std::uint32_t kElementBytes = 128;

enum class PlanResult { invalid_scale, invalid_native, baseline, unchanged, increase };

struct CapacityPlan {
  PlanResult result = PlanResult::invalid_native;
  std::uint32_t effective = 0;

  bool valid() const {
    return result != PlanResult::invalid_scale && result != PlanResult::invalid_native;
  }
};

inline std::optional<unsigned> parse_scale(std::string_view value) {
  if (value.empty() || value.size() > 2 || value.front() == '0') {
    return std::nullopt;
  }
  unsigned scale = 0;
  for (char digit : value) {
    if (digit < '0' || digit > '9') {
      return std::nullopt;
    }
    scale = scale * 10 + unsigned(digit - '0');
  }
  if (scale > kMaximumScale) {
    return std::nullopt;
  }
  return scale;
}

inline constexpr std::uint64_t allocation_bytes(std::uint32_t features) {
  return std::uint64_t{features} * kElementBytes;
}

inline constexpr std::uint32_t blocks(std::uint32_t features) {
  return features / kBlockFeatures;
}

inline constexpr bool valid_capacity(std::uint32_t features) {
  // Whole blocks only, block zero is reserved, and the GPU allocator rounds its unsigned 32-bit
  // byte width up to a 256-byte boundary.
  return features % kBlockFeatures == 0 && blocks(features) >= 2 &&
         allocation_bytes(features) + 255 <= UINT32_MAX;
}

inline CapacityPlan make_plan(unsigned scale, std::uint32_t native_features) {
  if (scale < 1 || scale > kMaximumScale) {
    return {PlanResult::invalid_scale, 0};
  }
  if (!valid_capacity(native_features)) {
    return {PlanResult::invalid_native, 0};
  }
  if (scale == 1) {
    return {PlanResult::baseline, native_features};
  }
  const auto floor = kNativeFeatures * scale;
  if (native_features >= floor) {
    return {PlanResult::unchanged, native_features};
  }
  return {PlanResult::increase, floor};
}

// The native constructor sets its block count and reserves block zero for "no features".
inline bool matches_cpu_allocation(std::uint32_t expected, std::uint32_t block_count,
                                   std::uint32_t next_block, std::uintptr_t begin,
                                   std::uintptr_t end) {
  return valid_capacity(expected) && block_count == blocks(expected) && next_block == 1 &&
         begin != 0 && begin <= end && end - begin == allocation_bytes(expected);
}

}  // namespace kcds::runtime::characters::features
