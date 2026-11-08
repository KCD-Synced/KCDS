#include "server_pings.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace kcds::runtime::session::pings {
namespace {

constexpr int kParallel = 8;
constexpr int kTimeoutMs = 1500;
constexpr std::size_t kMostServers = 256;

std::mutex state_mutex;
std::uint64_t started_generation = 0;
bool running = false;
std::optional<std::map<std::string, int>> finished;

// Milliseconds a TCP connect to host:port takes, or -1.
int measure(const std::string& address) {
  const std::size_t colon = address.rfind(':');
  if (colon == std::string::npos || colon == 0) {
    return -1;
  }
  const std::string host = address.substr(0, colon);
  const std::string port = address.substr(colon + 1);
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* found = nullptr;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0 || !found) {
    return -1;
  }
  int result = -1;
  const SOCKET socket_handle = socket(found->ai_family, SOCK_STREAM, IPPROTO_TCP);
  if (socket_handle != INVALID_SOCKET) {
    u_long non_blocking = 1;
    ioctlsocket(socket_handle, FIONBIO, &non_blocking);
    const auto began = std::chrono::steady_clock::now();
    connect(socket_handle, found->ai_addr, int(found->ai_addrlen));
    fd_set written;
    FD_ZERO(&written);
    FD_SET(socket_handle, &written);
    fd_set failed;
    FD_ZERO(&failed);
    FD_SET(socket_handle, &failed);
    timeval wait{kTimeoutMs / 1000, (kTimeoutMs % 1000) * 1000};
    if (select(0, nullptr, &written, &failed, &wait) > 0 && FD_ISSET(socket_handle, &written)) {
      const auto took = std::chrono::steady_clock::now() - began;
      result = int(std::chrono::duration_cast<std::chrono::milliseconds>(took).count());
    }
    closesocket(socket_handle);
  }
  freeaddrinfo(found);
  return result;
}

void measure_all(std::vector<std::string> addresses) {
  std::map<std::string, int> results;
  std::mutex results_mutex;
  std::atomic<std::size_t> next{0};
  std::vector<std::thread> workers;
  for (int i = 0; i < kParallel; ++i) {
    workers.emplace_back([&] {
      for (std::size_t at = next++; at < addresses.size(); at = next++) {
        const int ms = measure(addresses[at]);
        if (ms >= 0) {
          std::lock_guard<std::mutex> lock(results_mutex);
          results[addresses[at]] = ms;
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  std::lock_guard<std::mutex> lock(state_mutex);
  finished = std::move(results);
  running = false;
}

}  // namespace

void start(std::uint64_t generation, std::vector<std::string> addresses) {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (running || generation == started_generation || addresses.empty()) {
    return;
  }
  if (addresses.size() > kMostServers) {
    addresses.resize(kMostServers);
  }
  started_generation = generation;
  running = true;
  std::thread(measure_all, std::move(addresses)).detach();
}

std::optional<std::map<std::string, int>> take_results() {
  std::lock_guard<std::mutex> lock(state_mutex);
  std::optional<std::map<std::string, int>> results;
  results.swap(finished);
  return results;
}

}  // namespace kcds::runtime::session::pings
