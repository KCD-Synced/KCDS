#include "faint.hpp"

#include <cstring>

namespace kcds::runtime::knockout::faint {

bool holds_faint(const void* soul, const void* unconscious_vtable) {
  if (!soul || !unconscious_vtable) {
    return false;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(soul);
  const std::uint8_t* const* begin = nullptr;
  const std::uint8_t* const* end = nullptr;
  std::memcpy(&begin, bytes + kSoulBuffsBegin, sizeof(begin));
  std::memcpy(&end, bytes + kSoulBuffsEnd, sizeof(end));
  if (!begin) {
    return false;
  }
  for (auto it = begin; it != end; ++it) {
    const std::uint8_t* instance = *it;
    if (!instance) {
      continue;
    }
    const void* vtable = nullptr;
    std::memcpy(&vtable, instance, sizeof(vtable));
    if (vtable != unconscious_vtable) {
      continue;
    }
    std::uint32_t reason = 0;
    std::memcpy(&reason, instance + kUnconsciousReason, sizeof(reason));
    if (reason == kFaintReason) {
      return true;
    }
  }
  return false;
}

}  // namespace kcds::runtime::knockout::faint
