#pragma once
// Local network impairment for measurement tools: random datagram loss and a fixed
// one-way delay, applied to one socket in one direction. The load probe uses one instance per
// direction on each client socket, so a probe client sees `delay` added to both its upload and its
// download (round trip + 2 x delay) and `loss` applied independently in each direction.
//
// This emulates a path, not a real network: the delay has no jitter (datagrams keep their order),
// it is applied when the owner polls its socket (so it is rounded up to the owner's tick), and the
// queue is bounded. Portable and deterministic for a given seed, so it is unit tested.
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>

namespace kcd2s::local::netsim {

struct ImpairmentSettings {
  double loss_fraction = 0.0;  // 0..1 per datagram
  double delay_seconds = 0.0;  // one way
  std::size_t max_queued = 65536;
  std::uint64_t seed = 0x9e3779b97f4a7c15ull;

  bool active() const {
    return loss_fraction > 0.0 || delay_seconds > 0.0;
  }
};

struct ImpairmentCounters {
  std::uint64_t offered = 0;
  std::uint64_t dropped = 0;   // random loss
  std::uint64_t overflow = 0;  // queue full
  std::uint64_t delivered = 0;
};

template <typename Item>
class ImpairedQueue {
 public:
  explicit ImpairedQueue(const ImpairmentSettings& settings) : settings_(settings) {
    state_ = settings.seed != 0 ? settings.seed : 1;
  }

  // Offers one datagram at time `now`. It is lost, refused (queue full) or queued for delivery
  // at now + delay.
  void offer(double now, Item item) {
    counters_.offered += 1;
    if (settings_.loss_fraction > 0.0 && next_unit() < settings_.loss_fraction) {
      counters_.dropped += 1;
      return;
    }
    if (queue_.size() >= settings_.max_queued) {
      counters_.overflow += 1;
      return;
    }
    queue_.emplace_back(now + settings_.delay_seconds, std::move(item));
  }

  // Moves the oldest datagram whose delay elapsed by `now` into `out`.
  bool take_due(double now, Item& out) {
    if (queue_.empty() || queue_.front().first > now) {
      return false;
    }
    out = std::move(queue_.front().second);
    queue_.pop_front();
    counters_.delivered += 1;
    return true;
  }

  std::size_t queued() const {
    return queue_.size();
  }
  const ImpairmentCounters& counters() const {
    return counters_;
  }

 private:
  // xorshift64*: uniform in [0, 1).
  double next_unit() {
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    const std::uint64_t value = state_ * 0x2545f4914f6cdd1dull;
    return double(value >> 11) / double(std::uint64_t{1} << 53);
  }

  ImpairmentSettings settings_;
  std::uint64_t state_ = 1;
  std::deque<std::pair<double, Item>> queue_;
  ImpairmentCounters counters_;
};

}  // namespace kcd2s::local::netsim
