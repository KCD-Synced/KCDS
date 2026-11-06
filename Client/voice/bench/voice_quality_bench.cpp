// voice_quality_bench <speech.raw> <out.raw> <loss percent> <jitter ms> [noise level]: a recording
// of synthetic speech (48 kHz mono float) through the whole client path and a relay with loss and
// jitter, as both ends do it. The result goes to a file for an objective score, and CPU cost to
// stdout. Only for measuring: the game has no way to do this.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <vector>

#include "kcd2s/voice/frame.hpp"
#include "kcds/voice/voice_engine.hpp"

using namespace kcds::voice;

namespace {

std::vector<float> read_file(const char* path) {
  std::vector<float> samples;
  std::FILE* file = std::fopen(path, "rb");
  if (!file) {
    return samples;
  }
  std::fseek(file, 0, SEEK_END);
  const long bytes = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  samples.resize(std::size_t(bytes) / sizeof(float));
  std::fread(samples.data(), sizeof(float), samples.size(), file);
  std::fclose(file);
  return samples;
}

struct Delayed {
  int due_step = 0;
  std::vector<std::uint8_t> data;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr,
                 "usage: voice_quality_bench speech.raw out.raw loss_percent jitter_ms [noise]\n");
    return 2;
  }
  const std::vector<float> speech = read_file(argv[1]);
  if (speech.empty()) {
    std::fprintf(stderr, "no speech in %s\n", argv[1]);
    return 2;
  }
  const double loss = std::atof(argv[3]) / 100.0;
  const int jitter_steps = std::atoi(argv[4]) / 10;
  const float noise_level = argc > 5 ? float(std::atof(argv[5])) : 0.0f;
  const char* streams_text = std::getenv("KCDS_BENCH_STREAMS");
  const int streams = streams_text ? std::max(1, std::atoi(streams_text)) : 1;

  std::array<std::uint8_t, 32> key_a{};
  std::array<std::uint8_t, 32> key_b{};
  key_a.fill(1);
  key_b.fill(2);
  std::mt19937 random(12345);
  std::uniform_real_distribution<double> chance(0.0, 1.0);
  std::normal_distribution<float> noise(0.0f, noise_level > 0.0f ? noise_level : 1.0f);
  std::uniform_int_distribution<int> delay(0, std::max(jitter_steps, 0));

  std::deque<Delayed> network;
  int step = 0;
  std::uint64_t down_counter = 0;
  std::uint64_t bytes_up = 0;
  std::uint64_t packets_up = 0;

  VoiceEngine a([&](const std::vector<std::uint8_t>& up) {
    bytes_up += up.size();
    packets_up += 1;
    kcd2s::voice::VoiceHeader header;
    if (!kcd2s::voice::read_voice_header(up.data(), up.size(), header)) {
      return;
    }
    kcd2s::voice::Key key{};
    std::copy(key_a.begin(), key_a.end(), key.begin());
    std::vector<std::uint8_t> payload;
    if (!kcd2s::voice::open_voice(up.data(), up.size(), header, key, payload) || payload.empty()) {
      return;
    }
    if (chance(random) < loss) {
      return;
    }
    // The same voice from as many talkers as asked (KCDS_BENCH_STREAMS): the listener's cost of
    // that many streams at once.
    for (int talker = 0; talker < streams; ++talker) {
      kcd2s::voice::VoiceHeader down;
      down.tag = kcd2s::voice::kVoiceDownTag;
      down.id = 7 + std::uint32_t(talker);
      down.counter = ++down_counter;
      kcd2s::voice::Key listener{};
      std::copy(key_b.begin(), key_b.end(), listener.begin());
      Delayed item;
      item.due_step = step + delay(random);
      kcd2s::voice::seal_voice(down, listener, payload.data(), payload.size(), item.data);
      network.push_back(std::move(item));
    }
  });
  VoiceEngine b([](const std::vector<std::uint8_t>&) {});

  std::size_t position = 0;
  a.set_test_devices(
      [&](float* out, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
          float sample = position < speech.size() ? speech[position] : 0.0f;
          if (noise_level > 0.0f) {
            sample += noise(random);
          }
          out[i] = sample;
          ++position;
        }
      },
      [](const float*, std::size_t) {});
  std::vector<float> heard;
  heard.reserve(speech.size() + 96000);
  b.set_test_devices([](float* out, std::size_t count) { std::fill(out, out + count, 0.0f); },
                     [&](const float* stereo, std::size_t count) {
                       for (std::size_t i = 0; i < count; ++i) {
                         heard.push_back(0.5f * (stereo[i * 2] + stereo[i * 2 + 1]));
                       }
                     });
  a.set_active(true);
  b.set_active(true);
  a.set_session(501, key_a);
  b.set_session(502, key_b);
  a.set_tier(1, 10.0f);
  b.set_listener(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f});
  for (int talker = 0; talker < streams; ++talker) {
    b.set_talker_position(7 + std::uint32_t(talker), Vec3{float(talker) - 3.5f, 2.0f, 0.0f});
  }
  a.set_push_to_talk(true);

  const int steps = int((speech.size() + 48000) / 480);
  double a_seconds = 0.0;
  double b_seconds = 0.0;
  for (step = 0; step < steps; ++step) {
    const auto t0 = std::chrono::steady_clock::now();
    a.test_step();
    const auto t1 = std::chrono::steady_clock::now();
    std::stable_partition(network.begin(), network.end(),
                          [&](const Delayed& item) { return item.due_step <= step; });
    while (!network.empty() && network.front().due_step <= step) {
      b.on_datagram(network.front().data.data(), network.front().data.size());
      network.pop_front();
    }
    b.test_step();
    const auto t2 = std::chrono::steady_clock::now();
    a_seconds += std::chrono::duration<double>(t1 - t0).count();
    b_seconds += std::chrono::duration<double>(t2 - t1).count();
  }
  const double audio_seconds = double(steps) * 0.01;
  std::FILE* out = std::fopen(argv[2], "wb");
  if (out) {
    std::fwrite(heard.data(), sizeof(float), heard.size(), out);
    std::fclose(out);
  }
  const EngineStats stats = b.stats();
  std::printf("audio %.1f s, sent %llu frames, %.1f kbps on the wire with headers\n", audio_seconds,
              static_cast<unsigned long long>(packets_up),
              double(bytes_up + packets_up * 28) * 8.0 / 1000.0 / audio_seconds);
  std::printf("received %llu, concealed %llu, late %llu\n",
              static_cast<unsigned long long>(stats.frames_received),
              static_cast<unsigned long long>(stats.frames_concealed),
              static_cast<unsigned long long>(stats.frames_late));
  std::printf("cpu of one core: talker %.2f %%, listener of %d streams %.2f %%, hrtf %d\n",
              100.0 * a_seconds / audio_seconds, streams, 100.0 * b_seconds / audio_seconds,
              int(stats.hrtf));
  return 0;
}
