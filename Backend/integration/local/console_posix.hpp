#pragma once
// Server console commands from stdin on Linux and other POSIX systems: the same
// interface as console_win.hpp. Under systemd or Docker without a terminal stdin is empty or
// closed, and the reader simply ends.
#ifdef _WIN32
#error "console_posix.hpp is for POSIX systems; Windows uses console_win.hpp"
#endif

#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace kcd2s::local {

class ConsoleReader {
 public:
  ConsoleReader() {
    thread_ = std::thread([this] { read(); });
  }
  ~ConsoleReader() {
    active_ = false;
    if (thread_.joinable()) {
      thread_.join();
    }
  }
  ConsoleReader(const ConsoleReader&) = delete;
  ConsoleReader& operator=(const ConsoleReader&) = delete;

  std::vector<std::string> take() {
    std::lock_guard lock(mutex_);
    auto result = std::move(commands_);
    commands_.clear();
    return result;
  }

 private:
  void read() {
    std::string line;
    bool overflow = false;
    char bytes[1024];
    pollfd input{STDIN_FILENO, POLLIN, 0};
    while (active_) {
      input.revents = 0;
      const int ready = poll(&input, 1, 200);  // wakes up to see the stop request
      if (ready < 0 && errno != EINTR) {
        return;
      }
      if (ready <= 0) {
        continue;
      }
      const ssize_t count = ::read(STDIN_FILENO, bytes, sizeof(bytes));
      if (count == 0) {
        return;  // stdin closed (no terminal)
      }
      if (count < 0) {
        if (errno == EINTR || errno == EAGAIN) {
          continue;
        }
        return;
      }
      for (ssize_t i = 0; i < count; ++i) {
        const char value = bytes[i];
        if (value == '\n') {
          if (!line.empty() && line.back() == '\r') {
            line.pop_back();
          }
          if (!overflow && !line.empty()) {
            std::lock_guard lock(mutex_);
            if (commands_.size() < 64) {
              commands_.push_back(std::move(line));
            }
          }
          line.clear();
          overflow = false;
        } else if (!overflow) {
          if (line.size() < 1024) {
            line += value;
          } else {
            overflow = true;
          }
        }
      }
    }
  }
  std::atomic<bool> active_{true};
  std::thread thread_;
  std::mutex mutex_;
  std::vector<std::string> commands_;
};

}  // namespace kcd2s::local
