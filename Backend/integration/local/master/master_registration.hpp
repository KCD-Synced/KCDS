#pragma once
// Lists the local KCDS server in the master server's browser: a heartbeat on start,
// every kHeartbeatSeconds and soon after the player count changes, and a deregistration on stop.
// All HTTP runs on the registration's own thread; the tick loop only stores the player count, so
// a slow or unreachable master never delays the simulation. HTTP: WinHTTP on Windows, libcurl
// elsewhere.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include "kcd2s/platform/http_win.hpp"
#else
#include "kcd2s/platform/http_curl.hpp"
#endif
#include "kcd2s/platform/master_client.hpp"
#include "kcd2s/platform/server_browser.hpp"
#include "registration_settings.hpp"

namespace kcd2s::local::master {

#ifdef _WIN32
namespace http = platform::http_win;
#else
namespace http = platform::http_curl;
#endif

class MasterRegistration {
 public:
  static constexpr double kHeartbeatSeconds = 15.0;
  // A changed player count is announced after at most this many seconds.
  static constexpr double kChangeDelaySeconds = 3.0;

  MasterRegistration(RegistrationSettings settings, std::string map, int max_players, int protocol,
                     std::string version)
      : settings_(std::move(settings)) {
    entry_.id = settings_.id;
    entry_.name = settings_.name;
    entry_.address = settings_.address;
    entry_.image = settings_.image;
    entry_.password = settings_.password;
    // Queueing for full servers is; until then a full server refuses.
    entry_.queue = false;
    entry_.map = std::move(map);
    entry_.maxPlayers = max_players;
    entry_.protocol = protocol;
    entry_.version = std::move(version);
    worker_ = std::thread([this] { run(); });
  }

  MasterRegistration(const MasterRegistration&) = delete;
  MasterRegistration& operator=(const MasterRegistration&) = delete;

  // Deregisters (bounded by the HTTP timeouts) and stops the thread.
  ~MasterRegistration() {
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

  // The id the Master lists this server under (its heartbeat answer, {"id": "..."}), empty
  // before the first answer.
  std::string listing_id() const {
    std::lock_guard<std::mutex> lock(listing_mutex_);
    return listing_id_;
  }

 private:
  // The value of "id" in the heartbeat's answer: letters and digits only.
  static std::string answered_id(const std::string& body) {
    const std::string field = "\"id\":\"";
    const auto start = body.find(field);
    if (start == std::string::npos) {
      return {};
    }
    std::string id;
    for (std::size_t i = start + field.size(); i < body.size() && body[i] != '"'; ++i) {
      const char c = body[i];
      const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
      if (!plain || id.size() >= 128) {
        return {};
      }
      id.push_back(c);
    }
    return id;
  }

  platform::MasterClient client() {
    auto post = [this](const std::string& url, const std::string& body) {
      const auto result = http::request("POST", url, body, 5000, 4096);
      if (!result.error.empty() || result.status < 200 || result.status >= 300) {
        std::cerr << "MASTER " << url << ": "
                  << (result.error.empty() ? "HTTP " + std::to_string(result.status) : result.error)
                  << std::endl;
        return false;
      }
      if (const std::string id = answered_id(result.body); !id.empty()) {
        std::lock_guard<std::mutex> lock(listing_mutex_);
        listing_id_ = id;
      }
      return true;
    };
    return platform::MasterClient(settings_.master_url, nullptr, post);
  }

  void run() {
    const auto master = client();
    int announced_players = -1;
    bool listed = false;
    auto next_beat = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
      const int players = players_.load(std::memory_order_relaxed);
      const auto now = std::chrono::steady_clock::now();
      if (players != announced_players && announced_players >= 0) {
        const auto soon = now + std::chrono::milliseconds(int(kChangeDelaySeconds * 1000));
        if (soon < next_beat) {
          next_beat = soon;
        }
      }
      if (now >= next_beat) {
        entry_.players = players;
        lock.unlock();
        const bool ok = master.heartbeat(entry_);
        lock.lock();
        if (ok != listed || announced_players < 0) {
          std::cout << "MASTER " << (ok ? "listed" : "heartbeat failed") << " at "
                    << settings_.master_url << " as \"" << entry_.name << "\" address "
                    << entry_.address << std::endl;
        }
        listed = ok;
        announced_players = players;
        next_beat = now + std::chrono::milliseconds(int(kHeartbeatSeconds * 1000));
      }
      wake_.wait_for(lock, std::chrono::milliseconds(500));
    }
    lock.unlock();
    if (listed) {
      const bool removed = master.deregister(entry_.id);
      std::cout << "MASTER " << (removed ? "deregistered" : "deregistration failed") << std::endl;
    }
  }

  RegistrationSettings settings_;
  platform::ServerEntry entry_;
  std::atomic<int> players_{0};
  mutable std::mutex listing_mutex_;
  std::string listing_id_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace kcd2s::local::master
