#ifdef _WIN32
#define NOMINMAX
#endif
#include "kcds/voice/voice_engine.hpp"

#include "kcds/voice/echo_canceller.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "jitter_buffer.hpp"
#include "kcd2s/voice/frame.hpp"
#include "spatial.hpp"

extern "C" {
#include "opus.h"
#include "rnnoise.h"
}

#define MA_NO_ENCODING
#define MA_NO_DECODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

namespace kcds::voice {
namespace {

constexpr std::size_t kCaptureChunk = 480;  // 10 ms, RNNoise's frame
constexpr std::size_t kMaxTalkers = 8;
constexpr int kOpusBitrate = 32000;
// Measured with PESQ: 9 gives 4.33 at 2.2 % of a core, 5 gives 4.30 at 1.7 %, 3 gives 4.23 at 1.6
// %.
constexpr int kOpusComplexity = 5;
constexpr int kOpusExpectedLoss = 10;
// A frame's payload: the talker's sequence, their tier's range in decimetres, then Opus.
constexpr std::size_t kPayloadHeader = 4;
// A talker whose stream has ended is kept this long for a new start without a new decoder.
constexpr auto kTalkerIdle = std::chrono::seconds(10);
// A talker's mouth moves while their sound is heard: a played block louder than this (about
// -40 dBFS) and for this long after it. Silence that is still sent (push-to-talk held, a quiet
// pause) keeps the mouth shut.
constexpr float kSpokenLevel = 0.01f;
constexpr auto kSpokenHold = std::chrono::milliseconds(250);
// Without a running mix (no output device, or deaf) the packets alone tell who talks.
constexpr auto kMixStalled = std::chrono::milliseconds(500);
// Voice activation: open at this much voice probability, stay open this long after it falls.
constexpr float kVoiceOpen = 0.6f;
constexpr int kHangoverFrames = 40;  // 400 ms in 10 ms frames
// Automatic gain: aims a speaking voice here, and never raises by more than this.
constexpr float kAgcTarget = 0.12f;
constexpr float kAgcMaxGain = 6.0f;

float soft_clip(float sample) {
  if (sample > 1.0f) {
    return 1.0f;
  }
  if (sample < -1.0f) {
    return -1.0f;
  }
  return sample;
}

template <typename T>
class AudioFifo {
 public:
  explicit AudioFifo(std::size_t capacity) : data_(capacity) {
  }
  std::size_t size() const {
    return size_;
  }
  // Drops the oldest when full: the newest sound is the one worth having.
  void push(const T* values, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
      data_[(start_ + size_) % data_.size()] = values[i];
      if (size_ < data_.size()) {
        ++size_;
      } else {
        start_ = (start_ + 1) % data_.size();
      }
    }
  }
  std::size_t pop(T* values, std::size_t count) {
    const std::size_t taken = std::min(count, size_);
    for (std::size_t i = 0; i < taken; ++i) {
      values[i] = data_[(start_ + i) % data_.size()];
    }
    start_ = (start_ + taken) % data_.size();
    size_ -= taken;
    return taken;
  }
  void clear() {
    start_ = 0;
    size_ = 0;
  }

 private:
  std::vector<T> data_;
  std::size_t start_ = 0;
  std::size_t size_ = 0;
};

struct Talker {
  JitterBuffer jitter;
  OpusDecoder* decoder = nullptr;
  std::unique_ptr<Spatializer> spatial;
  VoiceFilter filter;
  TalkerEffect effect;
  Vec3 position{};
  bool placed = false;
  float range = 10.0f;
  int silent_blocks = 0;
  bool playing = false;
  std::chrono::steady_clock::time_point heard{};
  // The last played block with sound in it.
  std::chrono::steady_clock::time_point spoke{};

  ~Talker() {
    if (decoder) {
      opus_decoder_destroy(decoder);
    }
  }
};

}  // namespace

class VoiceEngine::Impl {
 public:
  explicit Impl(Sender sender) : sender_(std::move(sender)) {
    ma_context_init(nullptr, 0, nullptr, &context_);
    int error = 0;
    encoder_ = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &error);
    if (encoder_) {
      opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(kOpusBitrate));
      opus_encoder_ctl(encoder_, OPUS_SET_VBR(1));
      opus_encoder_ctl(encoder_, OPUS_SET_VBR_CONSTRAINT(1));
      opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(kOpusComplexity));
      opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
      opus_encoder_ctl(encoder_, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
      opus_encoder_ctl(encoder_, OPUS_SET_INBAND_FEC(1));
      opus_encoder_ctl(encoder_, OPUS_SET_PACKET_LOSS_PERC(kOpusExpectedLoss));
      opus_encoder_ctl(encoder_, OPUS_SET_DTX(1));
    }
    denoise_ = rnnoise_create(nullptr);
    pending_mono_.reserve(kBlockSamples);
  }

  ~Impl() {
    set_active(false);
    if (encoder_) {
      opus_encoder_destroy(encoder_);
    }
    if (denoise_) {
      rnnoise_destroy(denoise_);
    }
    ma_context_uninit(&context_);
  }

  void set_active(bool active) {
    if (active == active_.load()) {
      return;
    }
    active_.store(active);
    if (active) {
      {
        std::lock_guard<std::mutex> lock(mix_mutex_);
        factory_ = make_spatial_factory(hrtf_allowed_);
        hrtf_.store(factory_->is_hrtf());
      }
      if (!test_devices_) {
        open_devices();
        worker_ = std::thread([this] { work(); });
      }
    } else {
      wake_.notify_all();
      if (worker_.joinable()) {
        worker_.join();
      }
      close_devices();
      std::lock_guard<std::mutex> lock(mix_mutex_);
      talkers_.clear();
      factory_.reset();
    }
  }

  bool active() const {
    return active_.load();
  }

  void set_session(std::uint32_t session, const std::array<std::uint8_t, 32>& key) {
    std::lock_guard<std::mutex> lock(key_mutex_);
    session_ = session;
    key_ = key;
    keyed_ = true;
    up_counter_ = 0;
    replay_ = kcd2s::voice::ReplayWindow{};
  }

  void clear_session() {
    std::lock_guard<std::mutex> lock(key_mutex_);
    keyed_ = false;
    session_ = 0;
  }

  void open_devices() {
    std::string input;
    std::string output;
    {
      std::lock_guard<std::mutex> lock(device_mutex_);
      input = input_name_;
      output = output_name_;
    }
    ma_device_id id{};
    ma_device_config capture = ma_device_config_init(ma_device_type_capture);
    capture.capture.format = ma_format_f32;
    capture.capture.channels = 1;
    capture.sampleRate = 48000;
    capture.periodSizeInFrames = kCaptureChunk;
    capture.dataCallback = capture_callback;
    capture.pUserData = this;
    if (find_device(input, ma_device_type_capture, id)) {
      capture.capture.pDeviceID = &id;
    }
    has_input_.store(ma_device_init(&context_, &capture, &capture_device_) == MA_SUCCESS);
    if (has_input_.load() && ma_device_start(&capture_device_) != MA_SUCCESS) {
      ma_device_uninit(&capture_device_);
      has_input_.store(false);
    }
    ma_device_id out_id{};
    ma_device_config playback = ma_device_config_init(ma_device_type_playback);
    playback.playback.format = ma_format_f32;
    playback.playback.channels = 2;
    playback.sampleRate = 48000;
    playback.periodSizeInFrames = kCaptureChunk;
    playback.dataCallback = playback_callback;
    playback.pUserData = this;
    if (find_device(output, ma_device_type_playback, out_id)) {
      playback.playback.pDeviceID = &out_id;
    }
    has_output_.store(ma_device_init(&context_, &playback, &playback_device_) == MA_SUCCESS);
    if (has_output_.load() && ma_device_start(&playback_device_) != MA_SUCCESS) {
      ma_device_uninit(&playback_device_);
      has_output_.store(false);
    }
  }

  void close_devices() {
    if (has_input_.exchange(false)) {
      ma_device_uninit(&capture_device_);
    }
    if (has_output_.exchange(false)) {
      ma_device_uninit(&playback_device_);
    }
    std::lock_guard<std::mutex> lock(capture_mutex_);
    capture_fifo_.clear();
  }

  bool find_device(const std::string& name, ma_device_type type, ma_device_id& id) {
    if (name.empty()) {
      return false;
    }
    ma_device_info* playback = nullptr;
    ma_device_info* capture = nullptr;
    ma_uint32 playback_count = 0;
    ma_uint32 capture_count = 0;
    if (ma_context_get_devices(&context_, &playback, &playback_count, &capture, &capture_count) !=
        MA_SUCCESS) {
      return false;
    }
    const ma_device_info* devices = type == ma_device_type_capture ? capture : playback;
    const ma_uint32 count = type == ma_device_type_capture ? capture_count : playback_count;
    for (ma_uint32 i = 0; i < count; ++i) {
      if (name == devices[i].name) {
        id = devices[i].id;
        return true;
      }
    }
    return false;
  }

  std::vector<std::string> devices(ma_device_type type) {
    std::vector<std::string> names;
    ma_device_info* playback = nullptr;
    ma_device_info* capture = nullptr;
    ma_uint32 playback_count = 0;
    ma_uint32 capture_count = 0;
    if (ma_context_get_devices(&context_, &playback, &playback_count, &capture, &capture_count) !=
        MA_SUCCESS) {
      return names;
    }
    const ma_device_info* list = type == ma_device_type_capture ? capture : playback;
    const ma_uint32 count = type == ma_device_type_capture ? capture_count : playback_count;
    for (ma_uint32 i = 0; i < count; ++i) {
      names.emplace_back(list[i].name);
    }
    return names;
  }

  void reopen_devices() {
    if (active_.load() && !test_devices_) {
      close_devices();
      open_devices();
    }
  }

  static void capture_callback(ma_device* device, void*, const void* input, ma_uint32 frames) {
    auto* self = static_cast<Impl*>(device->pUserData);
    {
      std::lock_guard<std::mutex> lock(self->capture_mutex_);
      self->capture_fifo_.push(static_cast<const float*>(input), frames);
    }
    self->wake_.notify_one();
  }

  static void playback_callback(ma_device* device, void* output, const void*, ma_uint32 frames) {
    auto* self = static_cast<Impl*>(device->pUserData);
    self->fill_output(static_cast<float*>(output), frames);
    self->remember_played(static_cast<const float*>(output), frames);
  }

  // What went to the speakers, for the echo canceller. Stereo in, mono kept.
  void remember_played(const float* stereo, std::size_t frames) {
    if (!echo_cancellation_.load()) {
      return;
    }
    float mono[kCaptureChunk];
    std::size_t done = 0;
    while (done < frames) {
      const std::size_t count = std::min<std::size_t>(kCaptureChunk, frames - done);
      for (std::size_t i = 0; i < count; ++i) {
        mono[i] = 0.5f * (stereo[(done + i) * 2] + stereo[(done + i) * 2 + 1]);
      }
      echo_canceller_.played(mono, count);
      done += count;
    }
  }

  // The capture thread: each 10 ms of microphone through noise removal and gain, each 20 ms
  // encoded and sent.
  void work() {
    std::vector<float> chunk(kCaptureChunk);
    while (active_.load()) {
      {
        std::unique_lock<std::mutex> lock(capture_mutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(50),
                       [this] { return !active_.load() || capture_fifo_.size() >= kCaptureChunk; });
        if (!active_.load() || capture_fifo_.size() < kCaptureChunk) {
          continue;
        }
        capture_fifo_.pop(chunk.data(), kCaptureChunk);
      }
      process_chunk(chunk.data());
    }
  }

  void process_chunk(float* chunk) {
    // RNNoise takes samples at 16-bit scale.
    if (echo_cancellation_.load()) {
      echo_canceller_.cancel(chunk);
    }
    const float input_gain = input_volume_.load();
    float scaled[kCaptureChunk];
    for (std::size_t i = 0; i < kCaptureChunk; ++i) {
      scaled[i] = chunk[i] * input_gain * 32768.0f;
    }
    // Push to talk with the key up: nothing goes out, so the noise removal rests. The microphone's
    // level is still measured for the settings' meter.
    const bool resting = mode_.load() == TalkMode::PushToTalk && !push_to_talk_.load();
    float voice_probability = 1.0f;
    if (denoise_ && !resting) {
      voice_probability = rnnoise_process_frame(denoise_, scaled, scaled);
    }
    float energy = 0.0f;
    for (std::size_t i = 0; i < kCaptureChunk; ++i) {
      scaled[i] /= 32768.0f;
      energy += scaled[i] * scaled[i];
    }
    const float level = std::sqrt(energy / float(kCaptureChunk));
    input_level_.store(level);

    // Gain follows only while it is voice, slowly, and limits peaks.
    if (voice_probability > 0.5f && level > 0.001f) {
      const float wanted = std::min(kAgcMaxGain, kAgcTarget / level);
      agc_gain_ += (wanted - agc_gain_) * 0.01f;
    }
    for (std::size_t i = 0; i < kCaptureChunk; ++i) {
      scaled[i] = soft_clip(scaled[i] * agc_gain_);
    }

    const bool wanted = wants_to_send(voice_probability);
    transmitting_.store(wanted);
    if (!wanted) {
      pending_mono_.clear();
      if (was_sending_) {
        // The encoder's next frame starts clean, not from where the last word ended.
        opus_encoder_ctl(encoder_, OPUS_RESET_STATE);
        was_sending_ = false;
      }
      return;
    }
    was_sending_ = true;
    pending_mono_.insert(pending_mono_.end(), scaled, scaled + kCaptureChunk);
    if (pending_mono_.size() >= kBlockSamples) {
      send_block(pending_mono_.data(), std::uint8_t(voice_probability * 255.0f));
      pending_mono_.clear();
    }
  }

  bool wants_to_send(float voice_probability) {
    if (muted_.load() || !encoder_) {
      return false;
    }
    if (mode_.load() == TalkMode::PushToTalk) {
      return push_to_talk_.load();
    }
    if (voice_probability >= voice_threshold_.load()) {
      hangover_ = kHangoverFrames;
    } else if (hangover_ > 0) {
      --hangover_;
    }
    return hangover_ > 0;
  }

  void send_block(const float* mono, std::uint8_t loudness) {
    std::uint8_t encoded[kcd2s::voice::kMaxVoicePayload];
    const int size = opus_encode_float(encoder_, mono, int(kBlockSamples), encoded + kPayloadHeader,
                                       int(sizeof(encoded) - kPayloadHeader));
    if (size <= 0) {
      return;
    }
    const std::uint16_t sequence = sequence_++;
    const std::uint16_t decimetres =
        std::uint16_t(std::clamp(range_meters_.load() * 10.0f, 0.0f, 65535.0f));
    std::memcpy(encoded, &sequence, 2);
    std::memcpy(encoded + 2, &decimetres, 2);
    send_payload(encoded, std::size_t(size) + kPayloadHeader, loudness);
    frames_sent_.fetch_add(1);
  }

  void send_payload(const std::uint8_t* payload, std::size_t size, std::uint8_t loudness) {
    std::vector<std::uint8_t> sealed;
    {
      std::lock_guard<std::mutex> lock(key_mutex_);
      if (!keyed_) {
        return;
      }
      kcd2s::voice::VoiceHeader header;
      header.tag = kcd2s::voice::kVoiceUpTag;
      header.id = session_;
      header.counter = ++up_counter_;
      header.loudness = loudness;
      kcd2s::voice::Key key{};
      std::copy(key_.begin(), key_.end(), key.begin());
      if (!kcd2s::voice::seal_voice(header, key, payload, size, sealed)) {
        return;
      }
    }
    sender_(sealed);
  }

  // Says where this client can be reached; empty, so nobody hears it. Any thread.
  void say_hello() {
    send_payload(nullptr, 0, 0);
  }

  void on_datagram(const std::uint8_t* data, std::size_t size) {
    kcd2s::voice::VoiceHeader header;
    if (!active_.load() || deaf_.load() || !kcd2s::voice::read_voice_header(data, size, header) ||
        header.tag != kcd2s::voice::kVoiceDownTag) {
      return;
    }
    std::vector<std::uint8_t> payload;
    {
      std::lock_guard<std::mutex> lock(key_mutex_);
      if (!keyed_ || !replay_.may_accept(header.counter)) {
        return;
      }
      kcd2s::voice::Key key{};
      std::copy(key_.begin(), key_.end(), key.begin());
      if (!kcd2s::voice::open_voice(data, size, header, key, payload)) {
        return;
      }
      replay_.mark(header.counter);
    }
    if (payload.size() <= kPayloadHeader) {
      return;
    }
    std::uint16_t sequence = 0;
    std::uint16_t decimetres = 0;
    std::memcpy(&sequence, payload.data(), 2);
    std::memcpy(&decimetres, payload.data() + 2, 2);
    std::lock_guard<std::mutex> lock(mix_mutex_);
    Talker* talker = talker_for(header.id);
    if (!talker) {
      return;
    }
    talker->range = float(decimetres) / 10.0f;
    talker->heard = std::chrono::steady_clock::now();
    if (!talker->jitter.push(sequence, payload.data() + kPayloadHeader,
                             payload.size() - kPayloadHeader)) {
      frames_late_.fetch_add(1);
    }
    frames_received_.fetch_add(1);
  }

  // Under mix_mutex_. The relay sends at most 8 at once, and the quietest place goes to a newcomer.
  Talker* talker_for(std::uint32_t id) {
    const auto found = talkers_.find(id);
    if (found != talkers_.end()) {
      return found->second.get();
    }
    if (talkers_.size() >= kMaxTalkers) {
      auto oldest = talkers_.end();
      for (auto it = talkers_.begin(); it != talkers_.end(); ++it) {
        if (oldest == talkers_.end() || it->second->heard < oldest->second->heard) {
          oldest = it;
        }
      }
      if (oldest != talkers_.end()) {
        talkers_.erase(oldest);
      }
    }
    if (!factory_) {
      return nullptr;
    }
    auto talker = std::make_unique<Talker>();
    int error = 0;
    talker->decoder = opus_decoder_create(48000, 1, &error);
    if (!talker->decoder) {
      return nullptr;
    }
    talker->spatial = factory_->make();
    const auto place = positions_.find(id);
    if (place != positions_.end()) {
      talker->position = place->second;
      talker->placed = true;
    }
    Talker* raw = talker.get();
    talkers_[id] = std::move(talker);
    return raw;
  }

  // The output device's callback: mixes a block when the last one is used up.
  void fill_output(float* output, ma_uint32 frames) {
    ma_uint32 done = 0;
    while (done < frames) {
      if (output_fifo_.size() < 2) {
        mix_block();
      }
      done += ma_uint32(output_fifo_.pop(output + done * 2, (frames - done) * 2) / 2);
    }
  }

  // 20 ms from each talker placed in space, then to the stereo output.
  void mix_block() {
    float left[kBlockSamples] = {};
    float right[kBlockSamples] = {};
    {
      std::lock_guard<std::mutex> lock(mix_mutex_);
      const float volume = output_volume_.load();
      const auto now = std::chrono::steady_clock::now();
      last_mix_ = now;
      float decoded[kBlockSamples];
      for (auto it = talkers_.begin(); it != talkers_.end();) {
        Talker& talker = *it->second;
        if (!deaf_.load() && talker.jitter.ready() && decode_next(talker, decoded)) {
          if (block_level(decoded) > kSpokenLevel) {
            talker.spoke = now;
          }
          const float dx = talker.position.x - listener_position_.x;
          const float dy = talker.position.y - listener_position_.y;
          const float dz = talker.position.z - listener_position_.z;
          const float distance = talker.placed ? std::sqrt(dx * dx + dy * dy + dz * dz) : 0.0f;
          const float gain = distance_gain(distance, talker.range) * volume;
          const Vec3 direction = talker.placed ? direction_to(listener_position_, listener_forward_,
                                                              listener_up_, talker.position)
                                               : Vec3{0.0f, 0.0f, -1.0f};
          talker.filter.process(decoded, kBlockSamples, talker.effect, float(kSampleRate));
          talker.spatial->process(decoded, direction, gain, left, right);
        }
        if (now - talker.heard > kTalkerIdle) {
          it = talkers_.erase(it);
        } else {
          ++it;
        }
      }
      streams_.store(std::uint32_t(talkers_.size()));
      echo_.process(left, right, kBlockSamples);
    }
    float stereo[kBlockSamples * 2];
    for (std::size_t i = 0; i < kBlockSamples; ++i) {
      stereo[i * 2] = soft_clip(left[i]);
      stereo[i * 2 + 1] = soft_clip(right[i]);
    }
    output_fifo_.push(stereo, kBlockSamples * 2);
  }

  // The RMS of one decoded block.
  static float block_level(const float* block) {
    float energy = 0.0f;
    for (std::size_t i = 0; i < kBlockSamples; ++i) {
      energy += block[i] * block[i];
    }
    return std::sqrt(energy / float(kBlockSamples));
  }

  // Under mix_mutex_. False when the stream has ended.
  bool decode_next(Talker& talker, float* decoded) {
    if (talker.jitter.empty()) {
      // Nothing left: a little concealment fades the end of a word out, then silence.
      if (talker.silent_blocks >= 3) {
        talker.jitter.restart();
        talker.silent_blocks = 0;
        return false;
      }
      talker.silent_blocks += 1;
      opus_decode_float(talker.decoder, nullptr, 0, decoded, int(kBlockSamples), 0);
      frames_concealed_.fetch_add(1);
      return true;
    }
    talker.silent_blocks = 0;
    JitterFrame frame = talker.jitter.pop();
    int got = 0;
    if (!frame.missing) {
      got = opus_decode_float(talker.decoder, frame.data.data(), opus_int32(frame.data.size()),
                              decoded, int(kBlockSamples), 0);
    } else if (frame.next) {
      got = opus_decode_float(talker.decoder, frame.next->data(), opus_int32(frame.next->size()),
                              decoded, int(kBlockSamples), 1);
      frames_concealed_.fetch_add(1);
    } else {
      got = opus_decode_float(talker.decoder, nullptr, 0, decoded, int(kBlockSamples), 0);
      frames_concealed_.fetch_add(1);
    }
    if (got <= 0) {
      std::memset(decoded, 0, sizeof(float) * kBlockSamples);
    }
    return true;
  }

  std::vector<std::string> input_devices() {
    return devices(ma_device_type_capture);
  }
  std::vector<std::string> output_devices() {
    return devices(ma_device_type_playback);
  }

  void test_step() {
    float chunk[kCaptureChunk];
    if (test_source_) {
      test_source_(chunk, kCaptureChunk);
      process_chunk(chunk);
    }
    float out[kCaptureChunk * 2];
    fill_output(out, kCaptureChunk);
    remember_played(out, kCaptureChunk);
    if (test_sink_) {
      test_sink_(out, kCaptureChunk);
    }
  }

  Sender sender_;
  std::atomic<bool> active_{false};
  std::atomic<TalkMode> mode_{TalkMode::PushToTalk};
  std::atomic<bool> push_to_talk_{false};
  std::atomic<bool> muted_{false};
  std::atomic<bool> deaf_{false};
  // Under mix_mutex_: when the output last mixed a block.
  std::chrono::steady_clock::time_point last_mix_{};
  std::atomic<bool> transmitting_{false};
  std::atomic<float> range_meters_{10.0f};
  std::atomic<float> input_volume_{1.0f};
  std::atomic<float> output_volume_{1.0f};
  std::atomic<float> input_level_{0.0f};
  std::atomic<float> voice_threshold_{kVoiceOpen};
  std::atomic<bool> echo_cancellation_{false};
  // 10 ms frames, echoes up to 200 ms (a room and the devices' own delay).
  EchoCanceller echo_canceller_{kCaptureChunk, 9600, 48000};
  std::atomic<bool> has_input_{false};
  std::atomic<bool> has_output_{false};
  std::atomic<bool> hrtf_{false};
  std::atomic<std::uint64_t> frames_sent_{0};
  std::atomic<std::uint64_t> frames_received_{0};
  std::atomic<std::uint64_t> frames_concealed_{0};
  std::atomic<std::uint64_t> frames_late_{0};
  std::atomic<std::uint32_t> streams_{0};
  bool hrtf_allowed_ = true;

  ma_context context_{};
  ma_device capture_device_{};
  ma_device playback_device_{};
  std::mutex device_mutex_;
  std::string input_name_;
  std::string output_name_;

  std::thread worker_;
  std::mutex capture_mutex_;
  std::condition_variable wake_;
  AudioFifo<float> capture_fifo_{48000};
  OpusEncoder* encoder_ = nullptr;
  DenoiseState* denoise_ = nullptr;
  float agc_gain_ = 1.0f;
  int hangover_ = 0;
  bool was_sending_ = false;
  std::uint16_t sequence_ = 0;
  std::vector<float> pending_mono_;

  std::mutex key_mutex_;
  std::array<std::uint8_t, 32> key_{};
  std::uint32_t session_ = 0;
  bool keyed_ = false;
  std::uint64_t up_counter_ = 0;
  kcd2s::voice::ReplayWindow replay_;

  std::mutex mix_mutex_;
  std::unique_ptr<SpatialFactory> factory_;
  std::unordered_map<std::uint32_t, std::unique_ptr<Talker>> talkers_;
  std::unordered_map<std::uint32_t, Vec3> positions_;
  Vec3 listener_position_{};
  Vec3 listener_forward_{0.0f, 1.0f, 0.0f};
  Vec3 listener_up_{0.0f, 0.0f, 1.0f};
  AudioFifo<float> output_fifo_{kBlockSamples * 4};
  RoomEcho echo_{float(kSampleRate)};

  TestSource test_source_;
  TestSink test_sink_;
  bool test_devices_ = false;
};

VoiceEngine::VoiceEngine(Sender sender) : impl_(std::make_unique<Impl>(std::move(sender))) {
}
VoiceEngine::~VoiceEngine() = default;

void VoiceEngine::set_active(bool active) {
  impl_->set_active(active);
}
bool VoiceEngine::active() const {
  return impl_->active();
}
void VoiceEngine::set_session(std::uint32_t session, const std::array<std::uint8_t, 32>& key) {
  impl_->set_session(session, key);
  impl_->say_hello();
}
void VoiceEngine::say_hello() {
  impl_->say_hello();
}
void VoiceEngine::clear_session() {
  impl_->clear_session();
}
void VoiceEngine::set_mode(TalkMode mode) {
  impl_->mode_.store(mode);
}
TalkMode VoiceEngine::mode() const {
  return impl_->mode_.load();
}
void VoiceEngine::set_push_to_talk(bool held) {
  impl_->push_to_talk_.store(held);
}
void VoiceEngine::set_tier(std::uint8_t, float range_meters) {
  impl_->range_meters_.store(range_meters);
}
void VoiceEngine::set_muted(bool muted) {
  impl_->muted_.store(muted);
}
void VoiceEngine::set_deaf(bool deaf) {
  impl_->deaf_.store(deaf);
}
void VoiceEngine::set_output_volume(float volume) {
  impl_->output_volume_.store(std::clamp(volume, 0.0f, 2.0f));
}
void VoiceEngine::set_input_volume(float volume) {
  impl_->input_volume_.store(std::clamp(volume, 0.0f, 4.0f));
}
void VoiceEngine::set_echo_cancellation(bool on) {
  if (on && !impl_->echo_cancellation_.load()) {
    impl_->echo_canceller_.reset();
  }
  impl_->echo_cancellation_.store(on);
}

void VoiceEngine::set_voice_threshold(float probability) {
  impl_->voice_threshold_.store(probability);
}

void VoiceEngine::set_input_device(const std::string& name) {
  {
    std::lock_guard<std::mutex> lock(impl_->device_mutex_);
    impl_->input_name_ = name;
  }
  impl_->reopen_devices();
}
void VoiceEngine::set_output_device(const std::string& name) {
  {
    std::lock_guard<std::mutex> lock(impl_->device_mutex_);
    impl_->output_name_ = name;
  }
  impl_->reopen_devices();
}
std::vector<std::string> VoiceEngine::input_devices() const {
  return impl_->input_devices();
}
std::vector<std::string> VoiceEngine::output_devices() const {
  return impl_->output_devices();
}
void VoiceEngine::set_listener(const Vec3& position, const Vec3& forward, const Vec3& up) {
  std::lock_guard<std::mutex> lock(impl_->mix_mutex_);
  impl_->listener_position_ = position;
  impl_->listener_forward_ = forward;
  impl_->listener_up_ = up;
}
void VoiceEngine::set_talker_position(std::uint32_t talker, const Vec3& position) {
  std::lock_guard<std::mutex> lock(impl_->mix_mutex_);
  impl_->positions_[talker] = position;
  const auto found = impl_->talkers_.find(talker);
  if (found != impl_->talkers_.end()) {
    found->second->position = position;
    found->second->placed = true;
  }
}
void VoiceEngine::set_talker_effect(std::uint32_t talker, const TalkerEffect& effect) {
  std::lock_guard<std::mutex> lock(impl_->mix_mutex_);
  const auto found = impl_->talkers_.find(talker);
  if (found != impl_->talkers_.end()) {
    found->second->effect = effect;
  }
}
void VoiceEngine::set_room(RoomPreset preset) {
  std::lock_guard<std::mutex> lock(impl_->mix_mutex_);
  impl_->echo_.set_preset(preset);
}
void VoiceEngine::forget_talker(std::uint32_t talker) {
  std::lock_guard<std::mutex> lock(impl_->mix_mutex_);
  impl_->positions_.erase(talker);
  impl_->talkers_.erase(talker);
}
std::vector<std::uint32_t> VoiceEngine::talkers() const {
  std::lock_guard<std::mutex> lock(impl_->mix_mutex_);
  std::vector<std::uint32_t> ids;
  const auto now = std::chrono::steady_clock::now();
  const bool mixing = !impl_->deaf_.load() && now - impl_->last_mix_ < kMixStalled;
  for (const auto& [id, talker] : impl_->talkers_) {
    if (mixing) {
      // What is heard now: the jitter buffer's delay included, silence left out.
      if (now - talker->spoke < kSpokenHold) {
        ids.push_back(id);
      }
    } else if (now - talker->heard < std::chrono::milliseconds(300)) {
      ids.push_back(id);
    }
  }
  return ids;
}
void VoiceEngine::on_datagram(const std::uint8_t* data, std::size_t size) {
  impl_->on_datagram(data, size);
}
EngineStats VoiceEngine::stats() const {
  EngineStats stats;
  stats.has_microphone = impl_->has_input_.load();
  stats.has_output = impl_->has_output_.load();
  stats.hrtf = impl_->hrtf_.load();
  stats.transmitting = impl_->transmitting_.load();
  stats.frames_sent = impl_->frames_sent_.load();
  stats.frames_received = impl_->frames_received_.load();
  stats.frames_concealed = impl_->frames_concealed_.load();
  stats.frames_late = impl_->frames_late_.load();
  stats.streams = impl_->streams_.load();
  stats.input_level = impl_->input_level_.load();
  return stats;
}
void VoiceEngine::set_test_devices(TestSource source, TestSink sink) {
  impl_->test_source_ = std::move(source);
  impl_->test_sink_ = std::move(sink);
  impl_->test_devices_ = true;
}
void VoiceEngine::test_step() {
  impl_->test_step();
}
void VoiceEngine::test_set_hrtf_allowed(bool allowed) {
  impl_->hrtf_allowed_ = allowed;
}

}  // namespace kcds::voice
