#include "kcds/voice/echo_canceller.hpp"

#include <algorithm>
#include <cmath>

#include "speex/speex_echo.h"

namespace kcds::voice {
namespace {

// What was played but not yet taken: about 200 ms. More means the microphone stopped.
constexpr std::size_t kMaxPlayedFrames = 20;

std::int16_t to_pcm(float sample) {
  const float scaled = std::clamp(sample, -1.0f, 1.0f) * 32767.0f;
  return static_cast<std::int16_t>(std::lround(scaled));
}

}  // namespace

EchoCanceller::EchoCanceller(std::size_t frame, std::size_t tail, int sample_rate)
    : frame_(frame), microphone_(frame), reference_(frame), cleaned_(frame) {
  state_ = speex_echo_state_init(int(frame), int(tail));
  if (state_) {
    int rate = sample_rate;
    speex_echo_ctl(state_, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
  }
}

EchoCanceller::~EchoCanceller() {
  if (state_) {
    speex_echo_state_destroy(state_);
  }
}

void EchoCanceller::played(const float* samples, std::size_t count) {
  std::lock_guard<std::mutex> lock(mutex_);
  played_.insert(played_.end(), samples, samples + count);
  const std::size_t limit = frame_ * kMaxPlayedFrames;
  if (played_.size() > limit) {
    played_.erase(played_.begin(), played_.begin() + std::ptrdiff_t(played_.size() - limit));
  }
}

void EchoCanceller::cancel(float* samples) {
  if (!state_) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < frame_; ++i) {
      float reference = 0.0f;
      if (!played_.empty()) {
        reference = played_.front();
        played_.pop_front();
      }
      reference_[i] = to_pcm(reference);
    }
  }
  for (std::size_t i = 0; i < frame_; ++i) {
    microphone_[i] = to_pcm(samples[i]);
  }
  speex_echo_cancellation(state_, microphone_.data(), reference_.data(), cleaned_.data());
  for (std::size_t i = 0; i < frame_; ++i) {
    samples[i] = float(cleaned_[i]) / 32767.0f;
  }
}

void EchoCanceller::reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  played_.clear();
  if (state_) {
    speex_echo_state_reset(state_);
  }
}

}  // namespace kcds::voice
