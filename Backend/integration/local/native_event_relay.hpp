#pragma once
#include <cstdint>
#include <optional>
#include <unordered_map>
#include "kcd2s/integration/combat.hpp"
namespace kcd2s::local {
// Native event allocation alone is not a committed hit. Callers also allocate
// previews, inspect their results, and destroy them without Submit.
// Access is serialized by the adapter's mutex; never hold it through native calls.
class NativeEventRelay {
  std::unordered_map<std::uintptr_t, integration::MeleeEvent> staged_;

 public:
  void stage(std::uintptr_t event, const integration::MeleeEvent& impact) {
    if (event)
      staged_[event] = impact;
  }
  std::optional<integration::MeleeEvent> submit(std::uintptr_t event) {
    auto it = staged_.find(event);
    if (it == staged_.end())
      return std::nullopt;
    auto impact = it->second;
    staged_.erase(it);
    return impact;
  }
  bool destroy(std::uintptr_t event) {
    return staged_.erase(event) != 0;
  }
  void clear() {
    staged_.clear();
  }
};
}  // namespace kcd2s::local
