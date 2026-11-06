// kcds-voice as a running thing: the voice socket, the control port and a thread for
// each game server's link. The program around it only reads its settings; tests run it in-process.
#ifndef KCD2S_VOICE_VOICE_DAEMON_HPP
#define KCD2S_VOICE_VOICE_DAEMON_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kcd2s/voice/net.hpp"
#include "kcd2s/voice/voice_tenants.hpp"

namespace kcd2s::voice {

struct VoiceDaemonOptions {
  std::string bind;
  // 0 for tests: the system chooses.
  std::uint16_t voice_port = 7777;
  std::uint16_t control_port = 7778;
  // server id -> its secret.
  std::map<std::string, std::string> tenants;
  std::function<void(const std::string&)> log;
};

class VoiceDaemon {
 public:
  static constexpr std::size_t kMaxLinks = 64;
  static constexpr std::uint64_t kSilenceLimitMs = 5000;

  explicit VoiceDaemon(VoiceDaemonOptions options);
  ~VoiceDaemon();
  VoiceDaemon(const VoiceDaemon&) = delete;
  VoiceDaemon& operator=(const VoiceDaemon&) = delete;

  // False when a port cannot be had.
  bool start();
  void stop();
  std::uint16_t voice_port() const {
    return voice_port_;
  }
  std::uint16_t control_port() const {
    return control_port_;
  }
  std::size_t tenant_count() const;
  std::size_t player_count(const std::string& server_id) const;
  bool tenant_enabled(const std::string& server_id) const;
  std::uint64_t datagrams_out() const {
    return datagrams_out_.load(std::memory_order_relaxed);
  }
  // With the IP and UDP headers.
  std::uint64_t bytes_out() const {
    return bytes_out_.load(std::memory_order_relaxed);
  }

 private:
  struct Link {
    std::thread thread;
    std::atomic<bool> finished{false};
  };

  void log(const std::string& text) const;
  void read_voice();
  void accept_links();
  void run_link(net::Handle socket);
  void tick_tenants();
  void reap_links(bool all);
  // A new link for this id ends the one before it.
  std::uint64_t claim(const std::string& server_id);
  bool is_current(const std::string& server_id, std::uint64_t generation) const;

  VoiceDaemonOptions options_;
  std::uint16_t voice_port_ = 0;
  std::uint16_t control_port_ = 0;
  net::Handle voice_socket_ = net::kNoSocket;
  net::Handle control_socket_ = net::kNoSocket;
  std::unique_ptr<VoiceTenants> tenants_;
  std::atomic<bool> stopping_{false};
  std::atomic<std::uint64_t> datagrams_out_{0};
  std::atomic<std::uint64_t> bytes_out_{0};
  std::thread voice_thread_;
  std::thread accept_thread_;
  std::thread tick_thread_;
  std::vector<std::unique_ptr<Link>> links_;
  mutable std::mutex generations_mutex_;
  std::map<std::string, std::uint64_t> generations_;
  std::uint64_t next_generation_ = 0;
};

}  // namespace kcd2s::voice

#endif
