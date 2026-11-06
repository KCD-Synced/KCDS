// The game server's end of the control link to a standalone voice server. A thread of
// its own connects, keeps the link and comes back after it was lost; the game tick only queues, so
// a slow or dead voice server cannot stall the game. What is told while there is no link is
// dropped: after each Welcome the caller says everything again.
#ifndef KCD2S_VOICE_REMOTE_RELAY_HPP
#define KCD2S_VOICE_REMOTE_RELAY_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "kcd2s/voice/control_link.hpp"
#include "kcd2s/voice/net.hpp"
#include "kcd2s/voice/relay_port.hpp"

namespace kcd2s::voice {

struct RemoteRelayOptions {
  // Where the game server reaches the voice server: 'host:port'.
  std::string url;
  std::string secret;
  std::string server_id;
  // Where players reach it, 'host:port', when not the url's host and the voice server's own port.
  std::string public_address;
  // Told what goes wrong and what comes back. From the link's thread.
  std::function<void(const std::string&)> log;
};

// What a new link learned.
struct RemoteWelcome {
  Key public_key{};
  std::uint32_t session_base = 0;
  // Where players send voice, in network order.
  Address endpoint{};
};

class RemoteRelay : public RelayPort {
 public:
  // Sends waiting at most, before the link is given up as too slow and made again.
  static constexpr std::size_t kMaxWaitingBytes = 4 * 1024 * 1024;
  static constexpr std::uint64_t kSilenceLimitMs = 5000;

  explicit RemoteRelay(RemoteRelayOptions options);
  ~RemoteRelay() override;
  RemoteRelay(const RemoteRelay&) = delete;
  RemoteRelay& operator=(const RemoteRelay&) = delete;

  void start();
  void stop();

  // Once for each new link. The caller then says everything again and ends with mark_synced.
  std::optional<RemoteWelcome> take_welcome();
  bool connected() const;

  void join(core::EntityId player, std::uint32_t session, const Key& client_key);
  void mark_synced();

  void remove_player(core::EntityId player) override;
  void set_position(core::EntityId player, const core::Vec3& position,
                    std::uint32_t world) override;
  void set_enabled(bool enabled) override;
  void set_tier_range(Tier tier, float meters) override;
  void set_tier(core::EntityId player, Tier tier) override;
  void set_muted(core::EntityId player, bool muted) override;
  void set_deaf(core::EntityId player, bool deaf) override;
  void set_listener_mute(core::EntityId listener, core::EntityId talker, bool muted) override;
  std::vector<TalkChange> take_talk_changes() override;

 private:
  void run();
  // One link, from connecting to losing it. True when it got as far as a Welcome.
  bool run_link(std::uint32_t ip, std::uint16_t port);
  void post(const ControlMessage& message);
  void log(const std::string& text) const;
  void wait_before_retry(int milliseconds);
  // What waits, as one sealed run of frames. Empty when nothing does.
  std::vector<std::uint8_t> take_outgoing(ControlSealer& sealer);
  void end_all_talking();

  RemoteRelayOptions options_;

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  bool stopping_ = false;
  bool connected_ = false;
  std::optional<RemoteWelcome> welcome_;
  std::vector<ControlMessage> waiting_;
  std::size_t waiting_bytes_ = 0;
  bool too_much_waiting_ = false;
  std::map<core::EntityId, ControlPosition> positions_;
  std::vector<TalkChange> talk_changes_;
  // The link's thread alone.
  std::set<core::EntityId> talking_;
  std::thread thread_;
};

}  // namespace kcd2s::voice

#endif
