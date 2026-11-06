// voice_relay_bench: the relay with its encryption at 1000 players, without sockets.
// Prints how much of one core it takes and how much it would send. A bench, never a pass or fail
// threshold.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

#include "kcd2s/voice/voice_server.hpp"

using kcd2s::core::Vec3;
using namespace kcd2s::voice;

namespace {

struct Scenario {
  const char* name;
  float spread_meters;
  int players;
  int talkers;
};

void run(const Scenario& scenario) {
  constexpr std::size_t kOpusBytes = 84;  // 32 kbps at 20 ms, with the talker's small header
  constexpr int kFramesPerSecond = 50;
  constexpr int kSeconds = 5;
  std::uint64_t now_ms = 1000;
  std::uint64_t bytes_out = 0;
  std::uint64_t datagrams_out = 0;
  VoiceServer server(
      RelayConfig{},
      [&](const Address&, const std::vector<std::uint8_t>& data) {
        bytes_out += data.size() + 28;
        datagrams_out += 1;
      },
      [&] { return now_ms; });

  std::mt19937 random(42);
  std::uniform_real_distribution<float> place(0.0f, scenario.spread_meters);
  Key key{};
  key.fill(7);
  for (int i = 1; i <= scenario.players; ++i) {
    const auto id = static_cast<kcd2s::core::EntityId>(i);
    server.add_player(id, std::uint32_t(i), key);
    server.set_position(id, Vec3{place(random), place(random), 0.0f}, 0);
    // Each one says hello, so it can be sent to.
    VoiceHeader hello;
    hello.tag = kVoiceUpTag;
    hello.id = std::uint32_t(i);
    hello.counter = 1;
    std::vector<std::uint8_t> sealed;
    seal_voice(hello, key, nullptr, 0, sealed);
    server.pump();
    server.offer(Address{std::uint32_t(i), 1000}, sealed.data(), sealed.size());
  }
  server.pump();

  std::vector<std::uint8_t> opus(kOpusBytes, 9);
  std::vector<std::vector<std::uint8_t>> frames(std::size_t(scenario.talkers));
  std::uint64_t counter = 1;
  double busy = 0.0;
  bytes_out = 0;
  datagrams_out = 0;
  for (int frame = 0; frame < kFramesPerSecond * kSeconds; ++frame) {
    ++counter;
    for (int talker = 0; talker < scenario.talkers; ++talker) {
      VoiceHeader header;
      header.tag = kVoiceUpTag;
      header.id = std::uint32_t(talker + 1);
      header.counter = counter;
      header.loudness = std::uint8_t(100 + talker % 100);
      seal_voice(header, key, opus.data(), opus.size(), frames[std::size_t(talker)]);
    }
    const auto started = std::chrono::steady_clock::now();
    for (int talker = 0; talker < scenario.talkers; ++talker) {
      const auto& data = frames[std::size_t(talker)];
      server.offer(Address{std::uint32_t(talker + 1), 1000}, data.data(), data.size());
    }
    server.pump();
    busy += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    now_ms += 20;
  }
  const VoiceCounters counters = server.counters();
  std::printf(
      "%-16s %4d players %4d talkers: %5.1f %% of one core, out %6.1f Mbps, %7.0f datagrams/s, "
      "%.1f listeners each, dropped %llu, refused %llu\n",
      scenario.name, scenario.players, scenario.talkers, 100.0 * busy / double(kSeconds),
      double(bytes_out) * 8.0 / 1e6 / double(kSeconds), double(datagrams_out) / double(kSeconds),
      double(datagrams_out) / std::max(1.0, double(counters.frames_in)),
      static_cast<unsigned long long>(counters.queue_drops),
      static_cast<unsigned long long>(counters.refused));
}

}  // namespace

int main() {
  const Scenario scenarios[] = {
      {"spread world", 2000.0f, 1000, 100}, {"spread, loud", 2000.0f, 1000, 300},
      {"town", 300.0f, 1000, 100},          {"crowd", 40.0f, 1000, 100},
      {"crowd, loud", 40.0f, 1000, 300},    {"crowd, worst", 20.0f, 1000, 1000},
  };
  for (const Scenario& scenario : scenarios) {
    run(scenario);
  }
  return 0;
}
