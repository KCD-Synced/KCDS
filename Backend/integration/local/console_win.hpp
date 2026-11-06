#pragma once

#include <Windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace kcd2s::local {

class ConsoleReader {
 public:
  ConsoleReader() : input_(GetStdHandle(STD_INPUT_HANDLE)) {
    if (input_ && input_ != INVALID_HANDLE_VALUE)
      thread_ = std::thread([this] { read(); });
  }
  ~ConsoleReader() {
    active_ = false;
    if (thread_.joinable()) {
      CancelSynchronousIo(thread_.native_handle());
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
    DWORD count = 0;
    char bytes[1024];
    while (active_ && ReadFile(input_, bytes, sizeof(bytes), &count, nullptr) && count) {
      for (DWORD i = 0; i < count; ++i) {
        const char value = bytes[i];
        if (value == '\n') {
          if (!line.empty() && line.back() == '\r')
            line.pop_back();
          if (!overflow && !line.empty()) {
            std::lock_guard lock(mutex_);
            if (commands_.size() < 64)
              commands_.push_back(std::move(line));
          }
          line.clear();
          overflow = false;
        } else if (!overflow) {
          if (line.size() < 1024)
            line += value;
          else
            overflow = true;
        }
      }
    }
  }
  HANDLE input_;
  std::atomic<bool> active_{true};
  std::thread thread_;
  std::mutex mutex_;
  std::vector<std::string> commands_;
};

}  // namespace kcd2s::local
