#pragma once
// The player count of a server that is not listed in the server browser. The website's count of
// players online includes such servers too: once a minute an unlisted server tells the KCDS Master
// how many players it has. The report holds a random id made for this run (so the Master counts
// each server once), the player count and the version; no name, address or player. The Master
// never lists the server for it. A server bound to a loopback address (a test server nobody else
// can reach) sends nothing.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#ifdef _WIN32
#include "kcd2s/platform/http_win.hpp"
#else
#include "kcd2s/platform/http_curl.hpp"
#endif
#include "registration_settings.hpp"

namespace kcd2s::local::master {

// Whether an unlisted server bound to bind_ip reports its player count.
inline bool reports_unlisted_count(const RegistrationSettings& settings,
                                   const std::string& bind_ip) {
  return !settings.enabled && !detail::loopback_host(bind_ip);
}

// 16 hex digits, new for every run of the server.
inline std::string unlisted_run_id() {
  std::random_device random;
  std::uint64_t value = (std::uint64_t(random()) << 32) ^ std::uint64_t(random());
  static const char kHex[] = "0123456789abcdef";
  std::string id;
  for (int i = 0; i < 16; ++i) {
    id.push_back(kHex[value & 0xF]);
    value >>= 4;
  }
  return id;
}

// The report's body: {"id":"<run id>","players":N,"version":"<version>"}.
inline std::string unlisted_count_body(const std::string& id, int players,
                                       const std::string& version) {
  std::string clean_version;
  for (const char c : version) {
    const bool plain = (c >= '0' && c <= '9') || c == '.';
    if (plain && clean_version.size() < 32) {
      clean_version.push_back(c);
    }
  }
  return "{\"id\":\"" + id + "\",\"players\":" + std::to_string(players < 0 ? 0 : players) +
         ",\"version\":\"" + clean_version + "\"}";
}

class UnlistedCount {
 public:
  static constexpr double kReportSeconds = 60.0;

  UnlistedCount(std::string version) : version_(std::move(version)), id_(unlisted_run_id()) {
    worker_ = std::thread([this] { run(); });
  }

  UnlistedCount(const UnlistedCount&) = delete;
  UnlistedCount& operator=(const UnlistedCount&) = delete;

  ~UnlistedCount() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  // Called from the tick loop: a plain store, no I/O.
  void set_players(int players) {
    players_.store(players, std::memory_order_relaxed);
  }

 private:
  void run() {
#ifdef _WIN32
    namespace http = platform::http_win;
#else
    namespace http = platform::http_curl;
#endif
    const std::string url = std::string(kDefaultMasterUrl) + "/v1/unlisted";
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
      const std::string body =
          unlisted_count_body(id_, players_.load(std::memory_order_relaxed), version_);
      lock.unlock();
      // A failed report is not retried sooner: the count is only a statistic.
      http::request("POST", url, body, 5000, 1024);
      lock.lock();
      wake_.wait_for(lock, std::chrono::milliseconds(int(kReportSeconds * 1000)),
                     [this] { return stopping_; });
    }
  }

  std::string version_;
  std::string id_;
  std::atomic<int> players_{0};
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace kcd2s::local::master
