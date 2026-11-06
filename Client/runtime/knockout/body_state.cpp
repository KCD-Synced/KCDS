#include "body_state.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace kcds::runtime::knockout {
namespace {

// WHGame 1.5.5, confirmed through live: C_Soul v7 / reads derived stat55,
// while tests cached HP <=0. Positive HP is necessary for an available live body,
// and does not by itself distinguish a conscious player from an immortal knockout.
constexpr std::size_t kSoulIsUnconscious = 7;
constexpr std::size_t kSoulHealth = 1800;

}  // namespace

BodyState observe_body(void* soul, void* tracked_soul, IsUnconscious uniquely_resolved) {
  if (!soul || soul != tracked_soul || !uniquely_resolved) {
    return BodyState::Unavailable;
  }
  void* const* table = nullptr;
  std::memcpy(&table, soul, sizeof(table));
  if (!table || table[kSoulIsUnconscious] != reinterpret_cast<void*>(uniquely_resolved)) {
    return BodyState::Unavailable;
  }
  float health = 0;
  std::memcpy(&health, static_cast<const std::uint8_t*>(soul) + kSoulHealth, sizeof(health));
  if (!std::isfinite(health) || !(health > 0.0f)) {
    return BodyState::Unavailable;
  }
  return uniquely_resolved(soul) ? BodyState::KnockedOut : BodyState::Awake;
}

}  // namespace kcds::runtime::knockout
