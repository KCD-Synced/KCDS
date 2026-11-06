// Job system: a fixed thread pool sized to the hardware, plus a
// parallel_for that fans a range across the workers and blocks until done. KCD2-Synced schedules
// its read-only fan-out work (notably the per-client snapshot build) onto this instead of spawning
// ad-hoc threads. Correctness-first: a mutex+condvar queue (not yet work-stealing); the interface
// stays the same if that is optimized later.
#ifndef KCD2S_CORE_THREADING_HPP
#define KCD2S_CORE_THREADING_HPP

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace kcd2s::core {

class ThreadPool {
 public:
  // n == 0 => hardware_concurrency (at least 1).
  explicit ThreadPool(unsigned n = 0);
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  std::size_t size() const {
    return workers_.size();
  }

  // Enqueue a fire-and-forget task.
  void enqueue(std::function<void()> task);

  // Run fn(i) for i in [0, count), blocking until all complete. Work is chunked across workers;
  // with a single worker (or a tiny count) it runs inline on the caller. The caller must ensure
  // fn(i) touches only disjoint state for distinct i (no internal locking is done for you).
  // An exception thrown by fn reaches the caller after every chunk has finished (the first one
  // if several threw; the rest of a throwing chunk is skipped).
  void parallel_for(std::size_t count, const std::function<void(std::size_t)>& fn);

 private:
  void worker_loop();

  std::vector<std::thread> workers_;
  std::queue<std::function<void()>> tasks_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
};

}  // namespace kcd2s::core

#endif  // KCD2S_CORE_THREADING_HPP
