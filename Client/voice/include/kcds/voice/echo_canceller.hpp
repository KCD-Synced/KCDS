// Takes what the speakers play back out of what the microphone hears, so a player on speakers is
// not heard by others with their voices in it (SpeexDSP's echo canceller, third_party/speexdsp).
// played() from the output's thread, cancel() from the capture thread.
#ifndef KCDS_VOICE_ECHO_CANCELLER_HPP
#define KCDS_VOICE_ECHO_CANCELLER_HPP

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

struct SpeexEchoState_;

namespace kcds::voice {

class EchoCanceller {
 public:
  // frame: samples per cancel() call; tail: the longest echo it learns, in samples.
  EchoCanceller(std::size_t frame, std::size_t tail, int sample_rate);
  ~EchoCanceller();
  EchoCanceller(const EchoCanceller&) = delete;
  EchoCanceller& operator=(const EchoCanceller&) = delete;

  // Mono samples, -1 to 1, as they went to the speakers.
  void played(const float* samples, std::size_t count);
  // One frame of the microphone, -1 to 1, in place.
  void cancel(float* samples);
  // Forgets what it learned and what was played (another device, a new session).
  void reset();

 private:
  std::size_t frame_;
  SpeexEchoState_* state_ = nullptr;
  std::mutex mutex_;
  std::deque<float> played_;
  std::vector<std::int16_t> microphone_;
  std::vector<std::int16_t> reference_;
  std::vector<std::int16_t> cleaned_;
};

}  // namespace kcds::voice

#endif
