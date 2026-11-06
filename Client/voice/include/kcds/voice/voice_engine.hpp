// The client's voice: microphone to network and network to speakers. It knows
// nothing of the game: the runtime gives it keys, positions and datagrams. All of its work runs on
// its own threads, never on the engine thread, and a failing device leaves the game playable.
#ifndef KCDS_VOICE_VOICE_ENGINE_HPP
#define KCDS_VOICE_VOICE_ENGINE_HPP

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "kcds/voice/voice_effects.hpp"

namespace kcds::voice {

enum class TalkMode : std::uint8_t { PushToTalk = 0, VoiceActivation = 1 };

struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

struct EngineStats {
  bool has_microphone = false;
  bool has_output = false;
  bool hrtf = false;
  bool transmitting = false;
  std::uint64_t frames_sent = 0;
  std::uint64_t frames_received = 0;
  std::uint64_t frames_concealed = 0;
  std::uint64_t frames_late = 0;
  std::uint32_t streams = 0;
  float input_level = 0.0f;
};

class VoiceEngine {
 public:
  using Sender = std::function<void(const std::vector<std::uint8_t>&)>;

  explicit VoiceEngine(Sender sender);
  ~VoiceEngine();
  VoiceEngine(const VoiceEngine&) = delete;
  VoiceEngine& operator=(const VoiceEngine&) = delete;

  // Opens the devices and starts the threads; off closes them all. Any thread.
  void set_active(bool active);
  bool active() const;

  // The session the server gave and the key both ends derived. Any thread.
  void set_session(std::uint32_t session, const std::array<std::uint8_t, 32>& key);
  void clear_session();
  // Tells the server where this client can be reached. Each second: it keeps a home router's
  // way open for voice, and the first ones can come before the server has the key.
  void say_hello();

  void set_mode(TalkMode mode);
  TalkMode mode() const;
  // The push-to-talk key is held.
  void set_push_to_talk(bool held);
  // The server's tier for this player, sent with each frame for the listener's falloff.
  void set_tier(std::uint8_t tier, float range_meters);
  // A server-side silence: nothing is sent.
  void set_muted(bool muted);
  void set_deaf(bool deaf);
  // Each talker's volume 0 to 2 on the listener's side; output volume for all.
  void set_output_volume(float volume);
  void set_input_volume(float volume);
  // A device's name, empty for the system's default. Takes effect at once.
  // Voice activation: how sure it must be that this is speech, 0 to 1.
  void set_voice_threshold(float probability);
  // Takes the speakers' sound back out of the microphone, for players without headphones.
  void set_echo_cancellation(bool on);
  void set_input_device(const std::string& name);
  void set_output_device(const std::string& name);
  std::vector<std::string> input_devices() const;
  std::vector<std::string> output_devices() const;

  // From the engine thread, as often as it likes: where the listener is and faces, and where a
  // talker's body is. Sizes are small and copies quick.
  void set_listener(const Vec3& position, const Vec3& forward, const Vec3& up);
  void set_talker_position(std::uint32_t talker, const Vec3& position);
  void forget_talker(std::uint32_t talker);
  // How this talker sounds here, and the room the listener is in.
  void set_talker_effect(std::uint32_t talker, const TalkerEffect& effect);
  void set_room(RoomPreset preset);
  // The ids of talkers whose sound is heard now: a played block above about -40 dBFS in the last
  // 250 ms. Without a running output (no device, or deaf) a packet in the last 300 ms counts.
  std::vector<std::uint32_t> talkers() const;

  // From the network thread: a datagram with the voice tag.
  void on_datagram(const std::uint8_t* data, std::size_t size);

  EngineStats stats() const;

  // Test hooks. A source fills 480 mono samples at 48 kHz in place of the microphone, and a sink
  // gets each mixed stereo block in place of the speakers: tests measure with them; nothing is
  // stored. With them set, no device opens and test_step() drives the engine by hand.
  using TestSource = std::function<void(float*, std::size_t)>;
  using TestSink = std::function<void(const float*, std::size_t)>;
  void set_test_devices(TestSource source, TestSink sink);
  void test_step();
  void test_set_hrtf_allowed(bool allowed);

  class Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace kcds::voice

#endif
