// Portable budget policy for startup GPU skinning allocations.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace kcds::runtime::characters::skinning {

inline constexpr unsigned kDefaultScale = 4;
inline constexpr unsigned kMaximumScale = 8;
inline constexpr std::uint32_t kNativeBones = 131072;
inline constexpr std::uint32_t kNativeMorphs = 4096;
inline constexpr std::uint32_t kReservedElements = 768;
// Dispatch parameters encode offsets as floats. Leave room for a local joint index as well.
inline constexpr std::uint32_t kMaximumElements = (1u << 24) - kReservedElements;

struct Budgets {
  std::uint32_t bones = 0;
  std::uint32_t morphs = 0;

  bool operator==(const Budgets&) const = default;
};

enum class PlanResult { invalid_scale, invalid_native, baseline, unchanged, increase };

struct CapacityPlan {
  PlanResult result = PlanResult::invalid_native;
  Budgets effective;

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

inline constexpr bool valid_elements(std::int32_t count, std::uint32_t stride) {
  if (count <= static_cast<std::int32_t>(kReservedElements) ||
      count > static_cast<std::int32_t>(kMaximumElements)) {
    return false;
  }
  // Native allocation multiplies and rounds byte widths to 256 using 32-bit arithmetic.
  return std::uint64_t{static_cast<std::uint32_t>(count)} * stride + 255 <= UINT32_MAX;
}

inline CapacityPlan make_plan(unsigned scale, std::int32_t native_bones,
                              std::int32_t native_morphs) {
  if (scale < 1 || scale > kMaximumScale) {
    return {PlanResult::invalid_scale, {}};
  }
  if (!valid_elements(native_bones, 32) || !valid_elements(native_morphs, 8)) {
    return {PlanResult::invalid_native, {}};
  }
  const Budgets current{static_cast<std::uint32_t>(native_bones),
                        static_cast<std::uint32_t>(native_morphs)};
  if (scale == 1) {
    // Explicit baseline means no modification, including smaller user-configured budgets.
    return {PlanResult::baseline, current};
  }
  Budgets effective{kNativeBones * scale, kNativeMorphs * scale};
  if (current.bones > effective.bones) {
    effective.bones = current.bones;
  }
  if (current.morphs > effective.morphs) {
    effective.morphs = current.morphs;
  }
  return {effective == current ? PlanResult::unchanged : PlanResult::increase, effective};
}

inline constexpr std::uint64_t allocation_bytes(Budgets budgets) {
  return std::uint64_t{budgets.bones} * 32 * 2 + std::uint64_t{budgets.morphs} * 8;
}

}  // namespace kcds::runtime::characters::skinning
