#pragma once
// Plain monotonically increasing counters for the local UDP host and the load probe. Recording is
// a few integer additions, so the counters stay enabled; only reporting them is opt-in.
// Byte counts are UDP payload bytes handed to / returned by the socket (no IP/UDP headers).
#include <cstddef>
#include <cstdint>

namespace kcd2s::local::metrics {

struct TrafficCounters {
  std::uint64_t packets_sent = 0;
  std::uint64_t bytes_sent = 0;
  std::uint64_t packets_received = 0;
  std::uint64_t bytes_received = 0;
  // Datagrams the OS refused with "would block" (send buffer full). They never reached the wire.
  std::uint64_t send_would_block = 0;
  // Datagrams the OS refused for a passing network fault (no buffer space, network or host
  // unreachable, the adapter down a moment). Dropped like any lost datagram; the session goes on.
  std::uint64_t send_dropped = 0;
  // Largest datagram payload sent so far. A running maximum, not reset by traffic_delta.
  std::uint64_t largest_sent = 0;

  void record_sent(std::size_t bytes) {
    packets_sent += 1;
    bytes_sent += bytes;
    if (bytes > largest_sent) {
      largest_sent = bytes;
    }
  }

  void record_received(std::size_t bytes) {
    packets_received += 1;
    bytes_received += bytes;
  }

  void record_would_block() {
    send_would_block += 1;
  }

  void record_dropped() {
    send_dropped += 1;
  }
};

// Difference of two cumulative readings taken from the same counter (`later` after `earlier`).
inline TrafficCounters traffic_delta(const TrafficCounters& later, const TrafficCounters& earlier) {
  TrafficCounters delta;
  delta.packets_sent = later.packets_sent - earlier.packets_sent;
  delta.bytes_sent = later.bytes_sent - earlier.bytes_sent;
  delta.packets_received = later.packets_received - earlier.packets_received;
  delta.bytes_received = later.bytes_received - earlier.bytes_received;
  delta.send_would_block = later.send_would_block - earlier.send_would_block;
  delta.send_dropped = later.send_dropped - earlier.send_dropped;
  delta.largest_sent = later.largest_sent;
  return delta;
}

// Admission and queueing events observed by the local Host. All values are cumulative.
struct HostCounters {
  std::uint64_t joins = 0;
  std::uint64_t leaves = 0;
  // Handshake datagrams from non-members that arrived while the peer cap was reached. This counts
  // datagrams, not distinct refused clients.
  std::uint64_t handshakes_refused_at_capacity = 0;
  // CONNECT_REFUSED (server full) replies sent for valid over-capacity handshakes.
  std::uint64_t refusals_sent = 0;
  // Valid over-capacity handshakes left unanswered because the global refusal budget was spent.
  std::uint64_t refusals_rate_limited = 0;
  // Valid packets from a member dropped because its 256-entry inbox was full.
  std::uint64_t inbox_overflow_drops = 0;
  // Ticks whose socket drain stopped at the 1024-datagram budget with data possibly remaining.
  std::uint64_t receive_budget_exhausted_ticks = 0;
};

}  // namespace kcd2s::local::metrics
