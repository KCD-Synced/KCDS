#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <vector>

#include "jitter_buffer.hpp"
#include "kcd2s/testkit.hpp"
#include "kcd2s/voice/frame.hpp"
#include "kcds/voice/echo_canceller.hpp"
#include "kcds/voice/voice_engine.hpp"
#include "spatial.hpp"

using namespace kcds::voice;

namespace {

std::vector<std::uint8_t> frame_of(std::uint8_t value) {
  return std::vector<std::uint8_t>{value};
}

void push_seq(JitterBuffer& buffer, std::uint16_t sequence) {
  const auto data = frame_of(std::uint8_t(sequence));
  buffer.push(sequence, data.data(), data.size());
}

// Two engines joined by a relay that opens and seals as the server does.
struct Pair {
  std::array<std::uint8_t, 32> key_a{};
  std::array<std::uint8_t, 32> key_b{};
  std::uint64_t down_counter = 0;
  double phase = 0.0;
  float tone = 0.2f;
  std::vector<float> heard_left;
  std::vector<float> heard_right;
  std::unique_ptr<VoiceEngine> a;
  std::unique_ptr<VoiceEngine> b;

  Pair() {
    key_a.fill(1);
    key_b.fill(2);
    a = std::make_unique<VoiceEngine>([this](const std::vector<std::uint8_t>& up) { forward(up); });
    b = std::make_unique<VoiceEngine>([](const std::vector<std::uint8_t>&) {});
    a->set_test_devices(
        [this](float* out, std::size_t count) {
          for (std::size_t i = 0; i < count; ++i) {
            out[i] = tone * float(std::sin(phase));
            phase += 2.0 * 3.14159265 * 440.0 / 48000.0;
          }
        },
        [](const float*, std::size_t) {});
    b->set_test_devices([](float* out, std::size_t count) { std::fill(out, out + count, 0.0f); },
                        [this](const float* stereo, std::size_t count) {
                          for (std::size_t i = 0; i < count; ++i) {
                            heard_left.push_back(stereo[i * 2]);
                            heard_right.push_back(stereo[i * 2 + 1]);
                          }
                        });
    a->test_set_hrtf_allowed(false);
    b->test_set_hrtf_allowed(false);
    a->set_active(true);
    b->set_active(true);
    a->set_session(501, key_a);
    b->set_session(502, key_b);
    a->set_tier(1, 10.0f);
  }

  void forward(const std::vector<std::uint8_t>& up) {
    kcd2s::voice::VoiceHeader header;
    if (!kcd2s::voice::read_voice_header(up.data(), up.size(), header)) {
      return;
    }
    kcd2s::voice::Key key{};
    std::copy(key_a.begin(), key_a.end(), key.begin());
    std::vector<std::uint8_t> payload;
    if (!kcd2s::voice::open_voice(up.data(), up.size(), header, key, payload)) {
      return;
    }
    if (payload.empty()) {
      return;
    }
    kcd2s::voice::VoiceHeader down;
    down.tag = kcd2s::voice::kVoiceDownTag;
    down.id = 7;
    down.counter = ++down_counter;
    kcd2s::voice::Key listener{};
    std::copy(key_b.begin(), key_b.end(), listener.begin());
    std::vector<std::uint8_t> sealed;
    kcd2s::voice::seal_voice(down, listener, payload.data(), payload.size(), sealed);
    b->on_datagram(sealed.data(), sealed.size());
  }

  void run(int steps) {
    for (int i = 0; i < steps; ++i) {
      a->test_step();
      b->test_step();
    }
  }
};

float rms(const std::vector<float>& samples, std::size_t skip) {
  double sum = 0.0;
  std::size_t count = 0;
  for (std::size_t i = skip; i < samples.size(); ++i) {
    sum += samples[i] * samples[i];
    ++count;
  }
  return count ? float(std::sqrt(sum / double(count))) : 0.0f;
}

}  // namespace

KCD2S_TEST(voice_jitter, waits_for_its_delay_then_plays_in_order) {
  JitterBuffer buffer;
  KCD2S_CHECK(!buffer.ready());
  push_seq(buffer, 100);
  KCD2S_CHECK(!buffer.ready());
  push_seq(buffer, 101);
  KCD2S_CHECK(buffer.ready());
  KCD2S_CHECK_EQ(int(buffer.pop().data.at(0)), 100);
  KCD2S_CHECK_EQ(int(buffer.pop().data.at(0)), 101);
  KCD2S_CHECK(buffer.empty());
}

KCD2S_TEST(voice_jitter, frames_out_of_order_come_out_in_order) {
  JitterBuffer buffer;
  push_seq(buffer, 11);
  push_seq(buffer, 10);
  push_seq(buffer, 12);
  JitterBuffer ordered;
  ordered.push(10, frame_of(10).data(), 1);
  ordered.push(11, frame_of(11).data(), 1);
  KCD2S_CHECK(ordered.ready());
  KCD2S_CHECK(!ordered.pop().missing);
}

KCD2S_TEST(voice_jitter, a_lost_frame_offers_the_next_for_repair) {
  JitterBuffer buffer;
  push_seq(buffer, 1);
  push_seq(buffer, 2);
  push_seq(buffer, 4);
  KCD2S_CHECK(!buffer.pop().missing);
  KCD2S_CHECK(!buffer.pop().missing);
  const JitterFrame lost = buffer.pop();
  KCD2S_CHECK(lost.missing);
  KCD2S_CHECK(lost.next != nullptr);
  if (lost.next) {
    KCD2S_CHECK_EQ(int(lost.next->at(0)), 4);
  }
}

KCD2S_TEST(voice_jitter, late_frames_raise_the_delay_up_to_its_limit) {
  JitterBuffer buffer;
  push_seq(buffer, 10);
  push_seq(buffer, 11);
  buffer.pop();
  buffer.pop();
  const int before = buffer.target_frames();
  for (int i = 0; i < 40; ++i) {
    push_seq(buffer, 5);
  }
  KCD2S_CHECK(buffer.target_frames() > before);
  KCD2S_CHECK(buffer.target_frames() <= JitterBuffer::kMaxFrames);
  KCD2S_CHECK(buffer.late() >= 40u);
}

KCD2S_TEST(voice_jitter, sequence_numbers_wrap) {
  JitterBuffer buffer;
  push_seq(buffer, 65535);
  push_seq(buffer, 0);
  KCD2S_CHECK(buffer.ready());
  KCD2S_CHECK(!buffer.pop().missing);
  KCD2S_CHECK(!buffer.pop().missing);
}

KCD2S_TEST(voice_spatial, loudness_falls_with_distance_and_ends_at_the_range) {
  KCD2S_CHECK_EQ(distance_gain(0.5f, 10.0f), 1.0f);
  KCD2S_CHECK(distance_gain(5.0f, 10.0f) < distance_gain(2.0f, 10.0f));
  KCD2S_CHECK(distance_gain(9.9f, 10.0f) < 0.05f);
  KCD2S_CHECK_EQ(distance_gain(10.0f, 10.0f), 0.0f);
  KCD2S_CHECK_EQ(distance_gain(5.0f, 0.0f), 0.0f);
  float last = 2.0f;
  for (float d = 0.0f; d < 30.0f; d += 0.5f) {
    const float gain = distance_gain(d, 30.0f);
    KCD2S_CHECK(gain <= last);
    last = gain;
  }
}

KCD2S_TEST(voice_spatial, a_direction_is_in_the_listeners_frame) {
  const Vec3 listener{0.0f, 0.0f, 0.0f};
  const Vec3 forward{0.0f, 1.0f, 0.0f};
  const Vec3 up{0.0f, 0.0f, 1.0f};
  const Vec3 right = direction_to(listener, forward, up, Vec3{5.0f, 0.0f, 0.0f});
  KCD2S_CHECK(right.x > 0.99f);
  const Vec3 ahead = direction_to(listener, forward, up, Vec3{0.0f, 5.0f, 0.0f});
  KCD2S_CHECK(ahead.z < -0.99f);
  const Vec3 above = direction_to(listener, forward, up, Vec3{0.0f, 0.0f, 5.0f});
  KCD2S_CHECK(above.y > 0.99f);
}

KCD2S_TEST(voice_spatial, a_voice_on_the_right_is_louder_in_the_right_ear) {
  const auto factory = make_spatial_factory(false);
  KCD2S_CHECK(!factory->is_hrtf());
  auto spatial = factory->make();
  std::vector<float> mono(kBlockSamples);
  for (std::size_t i = 0; i < mono.size(); ++i) {
    mono[i] = 0.5f * std::sin(float(i) * 0.05f);
  }
  std::vector<float> left(kBlockSamples);
  std::vector<float> right(kBlockSamples);
  for (int block = 0; block < 5; ++block) {
    std::fill(left.begin(), left.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
    spatial->process(mono.data(), Vec3{1.0f, 0.0f, 0.0f}, 1.0f, left.data(), right.data());
  }
  KCD2S_CHECK(rms(right, 0) > rms(left, 0) * 1.3f);
}

KCD2S_TEST(voice_engine, push_to_talk_sends_only_while_held) {
  Pair pair;
  pair.run(50);
  KCD2S_CHECK_EQ(pair.a->stats().frames_sent, std::uint64_t{0});
  pair.a->set_push_to_talk(true);
  pair.run(50);
  const auto sent = pair.a->stats().frames_sent;
  KCD2S_CHECK(sent >= 24u && sent <= 26u);
  pair.a->set_push_to_talk(false);
  pair.run(50);
  KCD2S_CHECK_EQ(pair.a->stats().frames_sent, sent);
}

KCD2S_TEST(voice_engine, a_muted_player_sends_nothing) {
  Pair pair;
  pair.a->set_muted(true);
  pair.a->set_push_to_talk(true);
  pair.run(50);
  KCD2S_CHECK_EQ(pair.a->stats().frames_sent, std::uint64_t{0});
}

KCD2S_TEST(voice_engine, a_voice_comes_out_of_the_other_engine) {
  Pair pair;
  pair.b->set_talker_position(7, Vec3{0.0f, 1.0f, 0.0f});
  pair.a->set_push_to_talk(true);
  pair.run(300);
  KCD2S_CHECK(pair.b->stats().frames_received > 100u);
  KCD2S_CHECK(pair.b->stats().streams == 1u);
  KCD2S_CHECK_EQ(pair.b->talkers().size(), std::size_t{1});
}

KCD2S_TEST(voice_engine, a_talker_who_sends_silence_does_not_move_the_mouth) {
  Pair pair;
  pair.tone = 0.0f;
  pair.b->set_talker_position(7, Vec3{0.0f, 1.0f, 0.0f});
  pair.a->set_push_to_talk(true);
  pair.run(300);
  KCD2S_CHECK(pair.b->stats().frames_received > 0u);
  KCD2S_CHECK_EQ(pair.b->talkers().size(), std::size_t{0});
}

KCD2S_TEST(voice_engine, a_deaf_listener_and_a_listener_beyond_the_range_hear_nothing) {
  Pair deaf;
  deaf.b->set_deaf(true);
  deaf.a->set_push_to_talk(true);
  deaf.run(200);
  KCD2S_CHECK(rms(deaf.heard_left, 0) == 0.0f);

  Pair far_away;
  far_away.b->set_talker_position(7, Vec3{0.0f, 50.0f, 0.0f});
  far_away.a->set_push_to_talk(true);
  far_away.run(200);
  KCD2S_CHECK(rms(far_away.heard_left, 0) == 0.0f);
}

KCD2S_TEST(voice_engine, another_key_and_repeats_are_not_played) {
  Pair pair;
  pair.a->set_push_to_talk(true);
  std::array<std::uint8_t, 32> other{};
  other.fill(9);
  pair.b->set_session(502, other);
  pair.run(50);
  KCD2S_CHECK_EQ(pair.b->stats().frames_received, std::uint64_t{0});
}

KCD2S_TEST(voice_engine, without_a_session_nothing_is_sent_and_off_is_clean) {
  VoiceEngine engine([](const std::vector<std::uint8_t>&) {});
  engine.set_test_devices([](float* out, std::size_t count) { std::fill(out, out + count, 0.1f); },
                          [](const float*, std::size_t) {});
  engine.test_set_hrtf_allowed(false);
  engine.set_active(true);
  engine.set_push_to_talk(true);
  for (int i = 0; i < 20; ++i) {
    engine.test_step();
  }
  engine.set_active(false);
  engine.set_active(false);
  KCD2S_CHECK(!engine.active());
}

namespace {

std::vector<float> noise_block(std::size_t size) {
  std::vector<float> samples(size);
  unsigned state = 1;
  for (float& sample : samples) {
    state = state * 1664525u + 1013904223u;
    sample = float(int(state >> 16) - 32768) / 65536.0f;
  }
  return samples;
}

// The energy left after a first difference: how much of a block is high sounds.
float highs_of(const std::vector<float>& samples) {
  double sum = 0.0;
  for (std::size_t i = 1; i < samples.size(); ++i) {
    const double step = double(samples[i]) - double(samples[i - 1]);
    sum += step * step;
  }
  return float(std::sqrt(sum / double(samples.size())));
}

std::vector<float> filtered(const TalkerEffect& effect) {
  VoiceFilter filter;
  std::vector<float> block;
  for (int i = 0; i < 40; ++i) {
    block = noise_block(960);
    filter.process(block.data(), block.size(), effect, 48000.0f);
  }
  return block;
}

}  // namespace

KCD2S_TEST(voice_effects, a_wall_takes_the_highs_and_the_level) {
  const auto open = filtered(TalkerEffect{});
  TalkerEffect wall;
  wall.occlusion = 1.0f;
  const auto through = filtered(wall);
  KCD2S_CHECK(highs_of(through) < highs_of(open) * 0.3f);
  KCD2S_CHECK(rms(through, 0) < rms(open, 0) * 0.7f);
}

KCD2S_TEST(voice_effects, a_helmet_takes_highs_and_under_water_takes_more) {
  TalkerEffect helmet;
  helmet.muffle = 1.0f;
  TalkerEffect water;
  water.underwater = 1.0f;
  const auto open = filtered(TalkerEffect{});
  const auto muffled = filtered(helmet);
  const auto drowned = filtered(water);
  KCD2S_CHECK(highs_of(muffled) < highs_of(open));
  KCD2S_CHECK(highs_of(drowned) < highs_of(muffled));
}

KCD2S_TEST(voice_echo, what_the_speakers_play_is_taken_out_of_the_microphone) {
  constexpr std::size_t kFrame = 480;
  constexpr std::size_t kDelay = 1200;  // 25 ms from the speakers to the microphone
  kcds::voice::EchoCanceller canceller(kFrame, 9600, 48000);
  std::uint32_t noise = 12345;
  const auto next = [&noise] {
    noise = noise * 1664525u + 1013904223u;
    return (float((noise >> 9) & 0xFFFF) / 32768.0f - 1.0f) * 0.3f;
  };
  std::vector<float> history(kDelay, 0.0f);
  double heard_in = 0.0;
  double sent_out = 0.0;
  const auto started = std::chrono::steady_clock::now();
  constexpr int kFrames = 600;  // 6 s
  for (int frame = 0; frame < kFrames; ++frame) {
    std::vector<float> played(kFrame);
    std::vector<float> microphone(kFrame);
    for (std::size_t i = 0; i < kFrame; ++i) {
      played[i] = next();
      history.push_back(played[i]);
      microphone[i] = 0.5f * history[history.size() - 1 - kDelay];
    }
    canceller.played(played.data(), kFrame);
    double before = 0.0;
    for (const float s : microphone) {
      before += double(s) * s;
    }
    canceller.cancel(microphone.data());
    double after = 0.0;
    for (const float s : microphone) {
      after += double(s) * s;
    }
    // The last two seconds: after it learned the room.
    if (frame >= kFrames - 200) {
      heard_in += before;
      sent_out += after;
    }
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  const double reduction_db = 10.0 * std::log10(heard_in / std::max(sent_out, 1e-12));
  std::printf("echo reduced by %.1f dB, %.2f %% of one core\n", reduction_db,
              100.0 * seconds / 6.0);
  KCD2S_CHECK(reduction_db > 15.0);
}

KCD2S_TEST(voice_effects, a_talkers_state_becomes_their_effect) {
  TalkerEffect plain;
  apply_talker_state(TalkerState{}, plain);
  KCD2S_CHECK(plain.wobble == 0.0f && plain.weakness == 0.0f && plain.muffle == 0.0f);

  TalkerState tipsy;
  tipsy.drunkenness = kDrunkThreshold;
  TalkerEffect not_yet;
  apply_talker_state(tipsy, not_yet);
  KCD2S_CHECK(not_yet.wobble == 0.0f);

  TalkerState drunk;
  drunk.drunkenness = 1.0f;
  TalkerEffect wobbling;
  apply_talker_state(drunk, wobbling);
  KCD2S_CHECK(wobbling.wobble == 1.0f);

  TalkerState hurt;
  hurt.injury = 3.0f;
  TalkerEffect weak;
  apply_talker_state(hurt, weak);
  KCD2S_CHECK(weak.weakness == 1.0f);

  TalkerState breathless;
  breathless.stamina = 0.0f;
  TalkerEffect panting;
  apply_talker_state(breathless, panting);
  KCD2S_CHECK(panting.weakness == 0.5f);
  TalkerState rested;
  rested.stamina = kOutOfBreath;
  TalkerEffect steady;
  apply_talker_state(rested, steady);
  KCD2S_CHECK(steady.weakness == 0.0f);

  TalkerState helmet;
  helmet.visor_closed = true;
  helmet.riding = true;
  TalkerEffect closed;
  closed.masking = 0.4f;
  apply_talker_state(helmet, closed);
  KCD2S_CHECK(closed.muffle > 0.0f && closed.muffle < 1.0f);
  // Heavier rain stays what it was.
  KCD2S_CHECK(closed.masking == 0.4f);

  TalkerState out;
  out.unconscious = true;
  TalkerEffect mumble;
  apply_talker_state(out, mumble);
  KCD2S_CHECK(mumble.muffle == 1.0f && mumble.weakness == 1.0f);
}

KCD2S_TEST(voice_effects, a_wobble_stays_a_voice_and_never_goes_wild) {
  TalkerEffect drunk;
  drunk.wobble = 1.0f;
  const auto block = filtered(drunk);
  KCD2S_CHECK(rms(block, 0) > 0.01f);
  for (const float sample : block) {
    KCD2S_CHECK(std::isfinite(sample) && std::fabs(sample) <= 1.0f);
  }
}

KCD2S_TEST(voice_effects, outdoors_adds_nothing_and_a_church_rings_on) {
  RoomEcho outdoors(48000.0f);
  auto left = noise_block(960);
  auto right = left;
  const auto before = left;
  outdoors.process(left.data(), right.data(), 960);
  KCD2S_CHECK(left == before);

  RoomEcho church(48000.0f);
  church.set_preset(RoomPreset::Church);
  for (int i = 0; i < 60; ++i) {
    auto l = noise_block(960);
    auto r = l;
    church.process(l.data(), r.data(), 960);
  }
  std::vector<float> quiet_left(960, 0.0f);
  std::vector<float> quiet_right(960, 0.0f);
  church.process(quiet_left.data(), quiet_right.data(), 960);
  KCD2S_CHECK(rms(quiet_left, 0) > 0.0005f);
  for (const float sample : quiet_left) {
    KCD2S_CHECK(std::isfinite(sample) && std::fabs(sample) < 2.0f);
  }
}
