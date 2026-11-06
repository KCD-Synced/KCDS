#pragma once
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace kcd2s::local {

// Lines on their way to the resource host, written by their own thread. A busy host must not hold
// the simulation: writing to a full pipe blocks, and a tick held for 10 s times out every player.
// Order is kept and nothing is dropped. The queue is bounded: at the limit push() waits, which is
// what writing to the pipe directly would have done.
class ScriptOutbox {
 public:
  static constexpr std::size_t kMaxBytes = 64u << 20;
  using Writer = std::function<bool(const std::string&)>;
  using Closer = std::function<void()>;

  explicit ScriptOutbox(std::size_t max_bytes = kMaxBytes) : max_bytes_(max_bytes) {
  }
  ~ScriptOutbox() {
    finish();
    join();
  }
  ScriptOutbox(const ScriptOutbox&) = delete;
  ScriptOutbox& operator=(const ScriptOutbox&) = delete;

  // The writer returns false when the pipe is broken. The closer runs on the writing thread after
  // finish(), once everything queued was written.
  void start(Writer writer, Closer closer) {
    writer_ = std::move(writer);
    closer_ = std::move(closer);
    thread_ = std::thread([this] { run(); });
  }

  // Queues a line. False once the pipe is broken or finish() was called. Adds the time spent
  // waiting for room to waited_seconds.
  bool push(std::string line, double& waited_seconds) {
    std::unique_lock lock(mutex_);
    const auto fits = [&] {
      return failed_ || closing_ || queued_bytes_ + line.size() <= max_bytes_;
    };
    if (!fits()) {
      const auto started = std::chrono::steady_clock::now();
      room_.wait(lock, fits);
      const auto finished = std::chrono::steady_clock::now();
      waited_seconds += std::chrono::duration<double>(finished - started).count();
    }
    if (failed_ || closing_) {
      return false;
    }
    queued_bytes_ += line.size();
    lines_.push_back(std::move(line));
    work_.notify_one();
    return true;
  }

  // Bytes not yet written, including the line being written.
  std::size_t queued_bytes() const {
    std::lock_guard lock(mutex_);
    return queued_bytes_;
  }

  // No more lines. What is queued still gets written, then the closer runs.
  void finish() {
    std::lock_guard lock(mutex_);
    closing_ = true;
    work_.notify_all();
    room_.notify_all();
  }

  void join() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

 private:
  void run() {
    std::unique_lock lock(mutex_);
    while (true) {
      work_.wait(lock, [&] { return closing_ || failed_ || !lines_.empty(); });
      if (failed_ || lines_.empty()) {
        break;
      }
      const std::string line = std::move(lines_.front());
      lines_.pop_front();
      lock.unlock();
      const bool written = writer_(line);
      lock.lock();
      queued_bytes_ -= line.size();
      if (!written) {
        failed_ = true;
        lines_.clear();
        queued_bytes_ = 0;
      }
      room_.notify_all();
    }
    // A broken pipe keeps its handle until finish(), so that nobody uses a closed one.
    work_.wait(lock, [&] { return closing_; });
    lock.unlock();
    if (closer_) {
      closer_();
    }
  }

  const std::size_t max_bytes_;
  Writer writer_;
  Closer closer_;
  std::thread thread_;
  mutable std::mutex mutex_;
  std::condition_variable work_;
  std::condition_variable room_;
  std::deque<std::string> lines_;
  std::size_t queued_bytes_ = 0;
  bool closing_ = false;
  bool failed_ = false;
};

}  // namespace kcd2s::local
