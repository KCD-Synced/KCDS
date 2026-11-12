#pragma once
// What a talking bot says in voice chat: two seconds of generated sound (a soft noise that swells
// and fades like speech, never a recording), encoded once with Opus at the game's settings (48 kHz
// mono, 20 ms frames, 24 kbps) and sent over and over. Each bot seals the frames with its own
// session key (kcd2s::voice), as the game's voice engine does.
#include <opus.h>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "kcd2s/voice/frame.hpp"

namespace kcd2s::local::bots {

inline constexpr int kVoiceSampleRate = 48000;
inline constexpr int kVoiceFrameSamples = 960;  // 20 ms
inline constexpr double kVoiceFrameSeconds = 0.020;

class VoiceLoop {
 public:
  // `seed`: the noise's; the same seed gives the same frames.
  explicit VoiceLoop(std::uint64_t seed = 1) {
    int error = 0;
    OpusEncoder* encoder = opus_encoder_create(kVoiceSampleRate, 1, OPUS_APPLICATION_VOIP, &error);
    if (!encoder || error != OPUS_OK) {
      throw std::runtime_error("voice loop: no Opus encoder");
    }
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000));
    std::uint64_t state = seed * 0x9E3779B97F4A7C15ull + 1;
    std::vector<float> samples(kVoiceFrameSamples);
    for (int frame = 0; frame < 100; ++frame) {
      for (int i = 0; i < kVoiceFrameSamples; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        const double noise = double(state >> 11) / 9007199254740992.0 * 2.0 - 1.0;
        const double t = double(frame * kVoiceFrameSamples + i) / kVoiceSampleRate;
        // A syllable rhythm of about four a second.
        const double envelope = 0.5 + 0.5 * std::sin(2.0 * 3.14159265358979323846 * 4.0 * t);
        samples[std::size_t(i)] = float(0.08 * envelope * noise);
      }
      std::vector<std::uint8_t> packet(kcd2s::voice::kMaxVoicePayload);
      const int size = opus_encode_float(encoder, samples.data(), kVoiceFrameSamples, packet.data(),
                                         opus_int32(packet.size()));
      if (size <= 0) {
        opus_encoder_destroy(encoder);
        throw std::runtime_error("voice loop: Opus could not encode");
      }
      packet.resize(std::size_t(size));
      frames_.push_back(std::move(packet));
    }
    opus_encoder_destroy(encoder);
  }

  const std::vector<std::uint8_t>& frame(std::uint64_t index) const {
    return frames_[index % frames_.size()];
  }
  std::size_t size() const {
    return frames_.size();
  }

 private:
  std::vector<std::vector<std::uint8_t>> frames_;
};

}  // namespace kcd2s::local::bots
