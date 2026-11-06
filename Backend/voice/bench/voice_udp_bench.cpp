// The voice server's load with real UDP sockets: simulated talkers at 1000 players,
// built in (mode A: the relay and a socket) and standalone (mode B: kcds-voice's daemon and its
// control link). Prints numbers; never a pass or fail. CPU is the whole process's less the thread
// that plays the talkers, so it counts the socket reads and sends in the kernel too.
//
//   voice_udp_bench [seconds per scenario, default 10]
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "kcd2s/voice/net.hpp"
#include "kcd2s/voice/remote_relay.hpp"
#include "kcd2s/voice/voice_daemon.hpp"
#include "kcd2s/voice/voice_server.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#include <time.h>
#endif

using kcd2s::core::Vec3;
using namespace kcd2s::voice;

namespace {

constexpr std::size_t kOpusBytes = 80;  // 32 kbps at 20 ms
constexpr int kFramesPerSecond = 50;

#ifdef _WIN32
double seconds_of(const FILETIME& time) {
  ULARGE_INTEGER value;
  value.LowPart = time.dwLowDateTime;
  value.HighPart = time.dwHighDateTime;
  return double(value.QuadPart) / 1e7;
}
double process_cpu_seconds() {
  FILETIME created, ended, kernel, user;
  GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user);
  return seconds_of(kernel) + seconds_of(user);
}
double thread_cpu_seconds() {
  FILETIME created, ended, kernel, user;
  GetThreadTimes(GetCurrentThread(), &created, &ended, &kernel, &user);
  return seconds_of(kernel) + seconds_of(user);
}
#else
double process_cpu_seconds() {
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  return double(usage.ru_utime.tv_sec) + double(usage.ru_utime.tv_usec) / 1e6 +
         double(usage.ru_stime.tv_sec) + double(usage.ru_stime.tv_usec) / 1e6;
}
double thread_cpu_seconds() {
  timespec value{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value);
  return double(value.tv_sec) + double(value.tv_nsec) / 1e9;
}
#endif

std::uint64_t steady_ms() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

struct Player {
  net::Handle socket = net::kNoSocket;
  Key key{};
  Key public_key{};
  std::uint32_t session = 0;
  std::uint64_t counter = 0;
};

struct Scenario {
  const char* name;
  float spread_meters;
  int players;
  int talkers;
};

// Plays the talkers for that long and prints what the server's side cost.
void play(const char* mode, const Scenario& scenario, std::vector<Player>& players,
          const Address& to, int seconds, const std::function<std::uint64_t()>& datagrams_out,
          const std::function<std::uint64_t()>& bytes_out) {
  const std::vector<std::uint8_t> payload(kOpusBytes, 0x5A);
  std::vector<std::uint8_t> sealed;
  const auto send_frame = [&](Player& player) {
    VoiceHeader header;
    header.tag = kVoiceUpTag;
    header.id = player.session;
    header.counter = ++player.counter;
    header.loudness = 120;
    seal_voice(header, player.key, payload.data(), payload.size(), sealed);
    net::udp_send(player.socket, to, sealed.data(), sealed.size());
  };
  // Every listener says where it is; the relay then has them all.
  for (Player& player : players) {
    send_frame(player);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  const std::uint64_t out_before = datagrams_out();
  const std::uint64_t bytes_before = bytes_out();
  const double cpu_before = process_cpu_seconds();
  const double own_before = thread_cpu_seconds();
  const std::uint64_t started = steady_ms();
  std::uint64_t frames = 0;
  std::uint64_t late_ticks = 0;
  // On a schedule, not by sleeping 20 ms: a coarse system timer would play fewer frames.
  std::uint64_t next_tick = started;
  while (steady_ms() - started < std::uint64_t(seconds) * 1000) {
    if (steady_ms() < next_tick) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    if (steady_ms() - next_tick >= 20) {
      ++late_ticks;
    }
    next_tick += 20;
    for (int i = 0; i < scenario.talkers; ++i) {
      send_frame(players[std::size_t(i)]);
      ++frames;
    }
  }
  const double elapsed = double(steady_ms() - started) / 1000.0;
  const double own = thread_cpu_seconds() - own_before;
  const double cpu = process_cpu_seconds() - cpu_before - own;
  const double out = double(datagrams_out() - out_before);
  const double bytes = double(bytes_out() - bytes_before);
  std::printf(
      "%-8s %-14s %4d players %4d talkers: %6.1f %% of one core, out %6.1f Mbps, %7.0f datagrams/s "
      "(in %6.0f/s, generator late %llu)\n",
      mode, scenario.name, scenario.players, scenario.talkers, 100.0 * cpu / elapsed,
      bytes * 8.0 / elapsed / 1e6, out / elapsed, double(frames) / elapsed,
      static_cast<unsigned long long>(late_ticks));
  std::fflush(stdout);
}

std::vector<Player> make_players(int count, const Key& server_key, std::uint32_t base) {
  std::vector<Player> players(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    Player& player = players[std::size_t(i)];
    std::uint16_t port = 0;
    player.socket = net::udp_open("127.0.0.1", 0, port);
    const KeyPair keys = make_key_pair();
    player.public_key = keys.public_key;
    derive_session_key(keys.secret_key, server_key, keys.public_key, server_key, player.key);
    player.session = base + std::uint32_t(i) + 1;
  }
  return players;
}

void close_players(std::vector<Player>& players) {
  for (const Player& player : players) {
    net::close_socket(player.socket);
  }
}

// Mode A: the relay in this process with a socket of its own, as the game server has it.
void run_embedded(const Scenario& scenario, int seconds) {
  std::uint16_t port = 0;
  const net::Handle socket = net::udp_open("127.0.0.1", 0, port);
  std::atomic<std::uint64_t> sent{0};
  std::atomic<std::uint64_t> sent_bytes{0};
  VoiceServer relay(
      RelayConfig{},
      [&](const Address& to, const std::vector<std::uint8_t>& data) {
        net::udp_send(socket, to, data.data(), data.size());
        sent.fetch_add(1, std::memory_order_relaxed);
        sent_bytes.fetch_add(data.size() + 28, std::memory_order_relaxed);
      },
      steady_ms);
  relay.start();
  const KeyPair server_keys = make_key_pair();
  std::vector<Player> players = make_players(scenario.players, server_keys.public_key, 0);
  std::mt19937 random(7);
  std::uniform_real_distribution<float> place(0.0f, scenario.spread_meters);
  for (int i = 0; i < scenario.players; ++i) {
    Player& player = players[std::size_t(i)];
    Key session_key{};
    derive_session_key(server_keys.secret_key, player.public_key, player.public_key,
                       server_keys.public_key, session_key);
    const auto id = kcd2s::core::EntityId(i + 1);
    relay.add_player(id, player.session, session_key);
    relay.set_position(id, Vec3{place(random), place(random), 0.0f}, 0);
  }
  std::atomic<bool> stop{false};
  std::thread reader([&] {
    std::uint8_t buffer[kMaxVoiceDatagram + 1];
    while (!stop) {
      Address from;
      const int got = net::udp_recv(socket, buffer, sizeof(buffer), from, 50);
      if (got > 0) {
        relay.offer(from, buffer, std::size_t(got));
      }
    }
  });
  Address to{};
  to.ip = *net::resolve_ipv4("127.0.0.1");
  to.port = net::host_to_network_port(port);
  play(
      "built in", scenario, players, to, seconds, [&] { return sent.load(); },
      [&] { return sent_bytes.load(); });
  const VoiceCounters counters = relay.counters();
  if (counters.queue_drops != 0) {
    std::printf("         queue drops %llu\n",
                static_cast<unsigned long long>(counters.queue_drops));
  }
  stop = true;
  reader.join();
  relay.stop();
  close_players(players);
  net::close_socket(socket);
}

// Mode B: the whole kcds-voice daemon, and the control link that tells it the positions.
void run_standalone(const Scenario& scenario, int seconds) {
  VoiceDaemonOptions options;
  options.bind = "127.0.0.1";
  options.voice_port = 0;
  options.control_port = 0;
  options.tenants["bench"] = "bench secret";
  VoiceDaemon daemon(options);
  if (!daemon.start()) {
    std::printf("the daemon did not start\n");
    return;
  }
  RemoteRelayOptions link_options;
  link_options.url = "127.0.0.1:" + std::to_string(daemon.control_port());
  link_options.server_id = "bench";
  link_options.secret = "bench secret";
  RemoteRelay link(link_options);
  link.start();
  std::optional<RemoteWelcome> welcome;
  for (int i = 0; i < 500 && !welcome; ++i) {
    welcome = link.take_welcome();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!welcome) {
    std::printf("the link did not come up\n");
    return;
  }
  std::vector<Player> players =
      make_players(scenario.players, welcome->public_key, welcome->session_base);
  std::mt19937 random(7);
  std::uniform_real_distribution<float> place(0.0f, scenario.spread_meters);
  for (int i = 0; i < scenario.players; ++i) {
    const auto id = kcd2s::core::EntityId(i + 1);
    link.join(id, players[std::size_t(i)].session, players[std::size_t(i)].public_key);
    link.set_position(id, Vec3{place(random), place(random), 0.0f}, 0);
  }
  link.mark_synced();
  for (int i = 0; i < 500 && daemon.player_count("bench") < std::size_t(scenario.players); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  play(
      "kcds-voice", scenario, players, welcome->endpoint, seconds,
      [&] { return daemon.datagrams_out(); }, [&] { return daemon.bytes_out(); });
  link.stop();
  daemon.stop();
  close_players(players);
}

}  // namespace

int main(int argc, char** argv) {
  const int seconds = argc > 1 ? std::atoi(argv[1]) : 10;
  net::start_sockets();
  const Scenario scenarios[] = {
      {"spread world", 2000.0f, 1000, 100},
      {"town", 300.0f, 1000, 100},
      {"crowd", 40.0f, 1000, 100},
      {"crowd, loud", 40.0f, 1000, 300},
  };
  for (const Scenario& scenario : scenarios) {
    run_embedded(scenario, seconds);
    run_standalone(scenario, seconds);
  }
  return 0;
}
