#pragma once
// Convergence markers of kcd2s-load-probe. Every movement input carries a marker in the
// owner-authored motionMask field: the sender's steady clock in microseconds when the input was
// queued, made strictly increasing per sender. A receiver that applies a snapshot with a newer
// marker computes the convergence latency from its own steady clock, so senders and receivers may
// live in different generator processes. That only holds on one machine, where every process
// reads the same monotonic clock (QueryPerformanceCounter on Windows); separate generator hosts
// would need a synchronized clock. Portable and unit tested.
#include <chrono>
#include <cstdint>

namespace kcd2s::local::load {

inline std::int64_t marker_clock_microseconds() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::microseconds>(now).count();
}

// The next marker of a sender: the current clock, but always above the previous marker.
inline std::int64_t next_marker(std::int64_t previous, std::int64_t now_microseconds) {
  return now_microseconds > previous ? now_microseconds : previous + 1;
}

// Milliseconds from queueing the marker to `now_microseconds` (never negative).
inline double marker_age_milliseconds(std::int64_t marker, std::int64_t now_microseconds) {
  const std::int64_t age = now_microseconds - marker;
  return age > 0 ? static_cast<double>(age) / 1000.0 : 0.0;
}

}  // namespace kcd2s::local::load
