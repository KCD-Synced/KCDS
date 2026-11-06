// The server's voice relay as a running thing: datagrams in from the socket's reader thread, rules
// in from the game tick, frames out on its own thread. A full queue drops voice and
// never waits, so voice load cannot delay the game.
#ifndef KCD2S_VOICE_VOICE_SERVER_HPP
#define KCD2S_VOICE_VOICE_SERVER_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "kcd2s/voice/frame.hpp"
#include "kcd2s/voice/relay.hpp"
#include "kcd2s/voice/relay_port.hpp"

namespace kcd2s::voice {

// A peer's UDP address, as the host's socket gives it.
struct Address {
  std::uint32_t ip = 0;
  std::uint16_t port = 0;
  friend bool operator==(const Address& a, const Address& b) {
    return a.ip == b.ip && a.port == b.port;
  }
};

struct VoiceCounters {
  std::uint64_t frames_in = 0;
  std::uint64_t frames_out = 0;
  std::uint64_t refused = 0;
  std::uint64_t queue_drops = 0;
};

class VoiceServer : public RelayPort {
 public:
  using Sender = std::function<void(const Address&, const std::vector<std::uint8_t>&)>;
  using Clock = std::function<std::uint64_t()>;

  // Datagrams waiting at most: 1000 talkers at 50 frames a second for about 100 ms.
  static constexpr std::size_t kQueueLimit = 5000;
  // Waiting datagrams at which the relay is behind and its listeners hear fewer talkers, and
  // where it is well again. Fewer talkers cost nearly in proportion less.
  static constexpr std::size_t kBehindDatagrams = 1000;
  static constexpr std::size_t kCaughtUpDatagrams = 200;
  static constexpr std::size_t kSpeakersWhenBehind = 3;
  static constexpr std::size_t kSpeakersOverBudget = 4;
  // Datagrams out in a second that make about 250 Mbps. Only a very dense crowd comes here; its
  // listeners then hear fewer talkers until it is half that again.
  static constexpr std::uint64_t kOutBudget = 280000;

  // A talker silent this long has stopped talking.
  static constexpr std::uint64_t kTalkEndMs = 400;

  VoiceServer(RelayConfig config, Sender sender, Clock clock);
  ~VoiceServer() override;
  VoiceServer(const VoiceServer&) = delete;
  VoiceServer& operator=(const VoiceServer&) = delete;

  // The relay's thread. A server without it (tests) calls pump() itself.
  void start();
  void stop();
  // Handles what waits, and returns how many datagrams that was.
  std::size_t pump();

  // From the socket's reader thread: a datagram with a voice tag. Never blocks on voice work.
  void offer(const Address& from, const std::uint8_t* data, std::size_t size);

  // From the game tick. Each is applied on the relay's thread, in order.
  void add_player(core::EntityId player, std::uint32_t session, const Key& session_key);
  void remove_player(core::EntityId player) override;
  void set_position(core::EntityId player, const core::Vec3& position,
                    std::uint32_t world) override;
  void set_enabled(bool enabled) override;
  void set_tier_range(Tier tier, float meters) override;
  void set_tier(core::EntityId player, Tier tier) override;
  void set_muted(core::EntityId player, bool muted) override;
  void set_deaf(core::EntityId player, bool deaf) override;
  void set_listener_mute(core::EntityId listener, core::EntityId talker, bool muted) override;

  // Who started or stopped talking since the last call.
  std::vector<TalkChange> take_talk_changes() override;
  VoiceCounters counters() const;
  void set_out_budget(std::uint64_t datagrams_per_second) {
    out_budget_ = datagrams_per_second;
  }

 private:
  struct Session {
    core::EntityId player = core::kInvalidEntity;
    Key key{};
    ReplayWindow replay;
  };
  struct Peer {
    std::uint32_t session = 0;
    Key key{};
    Address address{};
    bool address_known = false;
    std::uint64_t down_counter = 0;
    std::uint64_t last_frame_ms = 0;
    bool talking = false;
  };
  struct Inbound {
    Address from;
    std::vector<std::uint8_t> data;
  };

  void post(std::function<void()> command);
  void handle(const Inbound& inbound, std::uint64_t now);
  void end_silent_talkers(std::uint64_t now);
  void run();

  Sender sender_;
  Clock clock_;
  Relay relay_;

  // The relay's thread alone.
  std::unordered_map<std::uint32_t, Session> sessions_;
  std::unordered_map<core::EntityId, Peer> peers_;
  std::vector<core::EntityId> listeners_;
  std::vector<std::uint8_t> payload_;
  std::vector<std::uint8_t> sealed_;
  bool behind_ = false;
  bool over_budget_ = false;
  std::uint64_t out_budget_ = kOutBudget;
  std::uint64_t out_this_second_ = 0;
  std::uint64_t second_started_ms_ = 0;

  // Shared with the other threads.
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Inbound> inbound_;
  std::vector<std::function<void()>> commands_;
  std::vector<TalkChange> talk_changes_;
  VoiceCounters counters_;
  bool stopping_ = false;
  std::thread thread_;
};

}  // namespace kcd2s::voice

#endif
