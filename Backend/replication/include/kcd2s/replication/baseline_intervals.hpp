// Group acknowledged ticks only when every modular field/epoch comparison has the same result.
#pragma once

#include <algorithm>
#include <concepts>
#include <limits>
#include <span>
#include <vector>

namespace kcd2s::replication {

template <std::unsigned_integral Tick>
class BaselineIntervals {
 public:
  void reset(std::span<const Tick> change_ticks) {
    boundaries_.clear();
    boundaries_.reserve(change_ticks.size() * 2);
    constexpr Tick half = Tick(Tick{1} << (std::numeric_limits<Tick>::digits - 1));
    for (const Tick changed : change_ticks) {
      // signed(changed - baseline) > 0 becomes false at changed, and becomes true one
      // tick after the opposite half-range boundary. Unsigned conversion preserves wrap.
      boundaries_.push_back(changed);
      boundaries_.push_back(Tick(changed - half + Tick{1}));
    }
    std::sort(boundaries_.begin(), boundaries_.end());
    boundaries_.erase(std::unique(boundaries_.begin(), boundaries_.end()), boundaries_.end());
  }

  std::size_t key(Tick baseline) const {
    return static_cast<std::size_t>(
        std::upper_bound(boundaries_.begin(), boundaries_.end(), baseline) - boundaries_.begin());
  }

  std::size_t boundary_count() const {
    return boundaries_.size();
  }

 private:
  std::vector<Tick> boundaries_;
};

}  // namespace kcd2s::replication
