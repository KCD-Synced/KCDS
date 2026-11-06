// Bounded startup allocation policy for native clothing material instances.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace kcds::runtime::characters::materials {

inline constexpr unsigned kDefaultScale = 8;
inline constexpr unsigned kMaximumScale = 8;
inline constexpr std::uint32_t kNativeCapacity = 2560;
inline constexpr std::uint32_t kMaximumCapacity = UINT16_MAX;
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
  if (value.size() != 1 || value.front() < '1' || value.front() > '8') {
    return std::nullopt;
  }
  return static_cast<unsigned>(value.front() - '0');
}

inline constexpr std::uint64_t allocation_bytes(std::uint32_t count) {
  return std::uint64_t{count} * kElementBytes;
}

inline constexpr bool valid_capacity(std::uint32_t count) {
  // Slot zero is reserved. The allocator and each material store unsigned 16-bit IDs.
  // The GPU allocator rounds its unsigned 32-bit byte width up to a 256-byte boundary.
  return count >= 1 && count <= kMaximumCapacity && allocation_bytes(count) + 255 <= UINT32_MAX;
}

inline CapacityPlan make_plan(unsigned scale, std::uint32_t native_count) {
  if (scale < 1 || scale > kMaximumScale) {
    return {PlanResult::invalid_scale, 0};
  }
  if (!valid_capacity(native_count)) {
    return {PlanResult::invalid_native, 0};
  }
  if (scale == 1) {
    return {PlanResult::baseline, native_count};
  }
  const auto floor = kNativeCapacity * scale;
  if (native_count >= floor) {
    return {PlanResult::unchanged, native_count};
  }
  return {PlanResult::increase, floor};
}

inline bool matches_cpu_allocation(std::uint32_t expected, std::uint16_t capacity,
                                   std::uintptr_t begin, std::uintptr_t end,
                                   std::uintptr_t allocated_end) {
  return valid_capacity(expected) && capacity == expected && begin != 0 && begin <= end &&
         end <= allocated_end && end - begin == allocation_bytes(expected) &&
         (allocated_end - begin) % kElementBytes == 0;
}

inline bool matches_gpu_allocation(std::uint32_t expected, std::uint32_t count,
                                   std::uint32_t stride, std::uintptr_t resource) {
  return valid_capacity(expected) && count == expected && stride == kElementBytes && resource != 0;
}

}  // namespace kcds::runtime::characters::materials
