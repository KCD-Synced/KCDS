#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "kcd2s/sdk/marshal.hpp"

namespace kcd2s::local {

class ScriptInboxOverflow : public std::runtime_error {
 public:
  ScriptInboxOverflow()
      : std::runtime_error(
            "RESOURCE IPC inbox capacity exceeded (16384 messages / 16 MiB); "
            "session failed to prevent silent command loss") {
  }
};

// Preserve child command order, including commands produced by a whole join burst.
// Store encoded bytes so the retained payload allocation has an exact bound. Decode on take().
// Never wait for the main thread here: it can be blocked writing to the child's other pipe.
// After overflow keep draining/discarding stdout, but every main-thread access fails explicitly.
class ScriptInbox {
 public:
  static constexpr std::size_t kMaxMessages = 16384;
  static constexpr std::size_t kMaxBytes = 16 * 1024 * 1024;

  struct State {
    std::size_t messages = 0;
    std::size_t bytes = 0;
    std::size_t peak_messages = 0;
    std::size_t peak_bytes = 0;
    bool failed = false;
  };

  bool push(std::vector<std::uint8_t> bytes) {
    std::lock_guard lock(mutex_);
    if (state_.failed) {
      return false;
    }
    // Charge capacity, not merely size: spare allocation must also fit the memory bound.
    if (inbox_.size() == kMaxMessages || bytes.capacity() > kMaxBytes - state_.bytes) {
      state_.failed = true;
      return false;
    }
    state_.bytes += bytes.capacity();
    inbox_.push_back(std::move(bytes));
    state_.messages = inbox_.size();
    state_.peak_messages = std::max(state_.peak_messages, state_.messages);
    state_.peak_bytes = std::max(state_.peak_bytes, state_.bytes);
    return true;
  }

  void check() const {
    std::lock_guard lock(mutex_);
    if (state_.failed) {
      throw ScriptInboxOverflow();
    }
  }

  State state() const {
    std::lock_guard lock(mutex_);
    return state_;
  }

  std::vector<sdk::Value> take() {
    std::vector<std::vector<std::uint8_t>> encoded;
    {
      std::lock_guard lock(mutex_);
      if (state_.failed) {
        throw ScriptInboxOverflow();
      }
      encoded.swap(inbox_);
      state_.messages = state_.bytes = 0;
    }
    std::vector<sdk::Value> result;
    result.reserve(encoded.size());
    for (const auto& bytes : encoded) {
      auto value = sdk::decode(bytes);
      if (value.ok && value.consumed == bytes.size()) {
        result.push_back(std::move(value.value));
      }
    }
    check();  // Do not return a healthy batch if another producer overflowed during decoding.
    return result;
  }

 private:
  mutable std::mutex mutex_;
  State state_;
  std::vector<std::vector<std::uint8_t>> inbox_;
};

}  // namespace kcd2s::local
