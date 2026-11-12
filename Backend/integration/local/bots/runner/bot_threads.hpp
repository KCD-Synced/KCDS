#pragma once
// The bots of a run on a few threads (about 50 bots each by default): every thread ticks its bots
// at the game client's rate on its own fixed schedule, and counts the ticks it could not keep (an
// overrun: the bot process, not the server, is then the bottleneck, and the result says so).
// Every second each thread publishes a small status for the live line.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "../bot/bot.hpp"
#ifdef _WIN32
#include "../../tick/tick_timer_win.hpp"
#else
#include "../../tick/tick_timer_posix.hpp"
#endif

namespace kcd2s::local::bots {

// What one thread says about its bots now.
struct ThreadStatus {
  std::size_t bots = 0;
  std::size_t playing = 0;
  std::size_t failed = 0;
  std::uint64_t bytes_sent = 0;
  std::uint64_t bytes_received = 0;
  std::uint64_t corrections = 0;
  std::uint64_t refused_joins = 0;
  std::uint64_t ticks = 0;
  std::uint64_t overruns = 0;  // ticks that started later than one period after their slot
  double busy_seconds = 0.0;   // time spent ticking bots
};

class BotThreads {
 public:
  BotThreads(std::vector<std::unique_ptr<Bot>> bots, unsigned threads, double tick_hz)
      : tick_hz_(tick_hz) {
    const unsigned most = unsigned(std::max<std::size_t>(1, bots.size()));
    threads = std::max(1u, std::min(threads, most));
    for (unsigned thread = 0; thread < threads; ++thread) {
      groups_.push_back(std::make_unique<Group>());
    }
    for (std::size_t index = 0; index < bots.size(); ++index) {
      groups_[index % threads]->bots.push_back(std::move(bots[index]));
    }
  }

  // Runs until `duration` seconds after `start` (clock_seconds()) or until `stop` is set; then
  // every bot leaves the server. Returns when every thread is done.
  void run(double start, double duration, const std::atomic<bool>& stop) {
    std::vector<std::thread> workers;
    for (auto& group : groups_) {
      Group* own = group.get();
      workers.emplace_back(
          [this, own, start, duration, &stop] { run_group(*own, start, duration, stop); });
    }
    for (auto& worker : workers) {
      worker.join();
    }
  }

  // Every thread's status, summed (any thread may still be running).
  ThreadStatus status() const {
    ThreadStatus total;
    for (const auto& group : groups_) {
      std::lock_guard<std::mutex> lock(group->status_mutex);
      const ThreadStatus& one = group->status;
      total.bots += one.bots;
      total.playing += one.playing;
      total.failed += one.failed;
      total.bytes_sent += one.bytes_sent;
      total.bytes_received += one.bytes_received;
      total.corrections += one.corrections;
      total.refused_joins += one.refused_joins;
      total.ticks += one.ticks;
      total.overruns += one.overruns;
      total.busy_seconds += one.busy_seconds;
    }
    return total;
  }

  std::size_t thread_count() const {
    return groups_.size();
  }

  // The bots, after run() returned.
  template <class Visit>
  void each_bot(Visit visit) const {
    for (const auto& group : groups_) {
      for (const auto& bot : group->bots) {
        visit(*bot);
      }
    }
  }

 private:
  struct Group {
    std::vector<std::unique_ptr<Bot>> bots;
    mutable std::mutex status_mutex;
    ThreadStatus status;
  };

  void publish(Group& group, std::uint64_t ticks, std::uint64_t overruns, double busy) {
    ThreadStatus status;
    status.bots = group.bots.size();
    status.ticks = ticks;
    status.overruns = overruns;
    status.busy_seconds = busy;
    for (const auto& bot : group.bots) {
      status.playing += bot->playing() ? 1 : 0;
      status.failed += bot->failed() ? 1 : 0;
      const auto traffic = bot->traffic();
      status.bytes_sent += traffic.bytes_sent;
      status.bytes_received += traffic.bytes_received;
      status.corrections += bot->stats().corrections;
      status.refused_joins += bot->stats().refused_joins;
    }
    std::lock_guard<std::mutex> lock(group.status_mutex);
    group.status = status;
  }

  void run_group(Group& group, double start, double duration, const std::atomic<bool>& stop) {
    tick::TickTimer timer;
    const double period = 1.0 / tick_hz_;
    double next = clock_seconds();
    double last = next;
    double next_publish = next;
    std::uint64_t ticks = 0;
    std::uint64_t overruns = 0;
    double busy = 0.0;
    while (!stop.load(std::memory_order_relaxed)) {
      const double begin = clock_seconds();
      const double now = begin - start;
      if (now >= duration) {
        break;
      }
      if (begin - next > period) {
        overruns += 1;
      }
      const double dt = std::min(0.25, begin - last);
      last = begin;
      for (auto& bot : group.bots) {
        bot->tick(now, begin, dt);
      }
      ticks += 1;
      const double end = clock_seconds();
      busy += end - begin;
      if (end >= next_publish) {
        next_publish = end + 1.0;
        publish(group, ticks, overruns, busy);
      }
      next += period;
      if (next < end) {
        next = end;  // fell behind: no burst to catch up
      } else {
        timer.wait(next - end);
      }
    }
    for (auto& bot : group.bots) {
      bot->finish();
    }
    publish(group, ticks, overruns, busy);
  }

  double tick_hz_;
  std::vector<std::unique_ptr<Group>> groups_;
};

}  // namespace kcd2s::local::bots
