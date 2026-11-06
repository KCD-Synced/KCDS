#include "kcd2s/core/threading.hpp"

#include <algorithm>
#include <exception>

namespace kcd2s::core {

ThreadPool::ThreadPool(unsigned n) {
  if (n == 0)
    n = std::max(1u, std::thread::hardware_concurrency());
  workers_.reserve(n);
  for (unsigned i = 0; i < n; ++i)
    workers_.emplace_back([this] { worker_loop(); });
}

ThreadPool::~ThreadPool() {
  {
    std::lock_guard<std::mutex> lk(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  for (auto& t : workers_)
    if (t.joinable())
      t.join();
}

void ThreadPool::worker_loop() {
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lk(mutex_);
      cv_.wait(lk, [this] { return stop_ || !tasks_.empty(); });
      if (stop_ && tasks_.empty())
        return;
      task = std::move(tasks_.front());
      tasks_.pop();
    }
    task();
  }
}

void ThreadPool::enqueue(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lk(mutex_);
    tasks_.push(std::move(task));
  }
  cv_.notify_one();
}

void ThreadPool::parallel_for(std::size_t count, const std::function<void(std::size_t)>& fn) {
  if (count == 0)
    return;
  const std::size_t workers = workers_.size();
  if (workers <= 1 || count == 1) {
    for (std::size_t i = 0; i < count; ++i)
      fn(i);
    return;
  }

  // Chunk the range so each worker gets a contiguous span (fewer tasks than indices).
  const std::size_t nchunks = std::min(count, workers);
  const std::size_t chunk = (count + nchunks - 1) / nchunks;

  std::mutex done_m;
  std::condition_variable done_cv;
  std::size_t remaining = nchunks;
  // The first exception of any chunk. A chunk that throws stops there; the others run to their
  // end, and the caller gets the exception once every chunk is done.
  std::exception_ptr failure;

  for (std::size_t c = 0; c < nchunks; ++c) {
    const std::size_t begin = c * chunk;
    const std::size_t end = std::min(begin + chunk, count);
    enqueue([&fn, begin, end, &done_m, &done_cv, &remaining, &failure] {
      std::exception_ptr error;
      try {
        for (std::size_t i = begin; i < end; ++i)
          fn(i);
      } catch (...) {
        error = std::current_exception();
      }
      std::lock_guard<std::mutex> lk(done_m);
      if (error && !failure)
        failure = error;
      if (--remaining == 0)
        done_cv.notify_one();
    });
  }

  {
    std::unique_lock<std::mutex> lk(done_m);
    done_cv.wait(lk, [&remaining] { return remaining == 0; });
  }
  if (failure)
    std::rethrow_exception(failure);
}

}  // namespace kcd2s::core
