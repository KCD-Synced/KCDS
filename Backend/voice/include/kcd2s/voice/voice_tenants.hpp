// A standalone voice server's game servers: each is a tenant with its own relay and
// secret check, its players in their own range of session numbers. No sockets here: kcds-voice has
// those, and tests drive this directly.
#ifndef KCD2S_VOICE_VOICE_TENANTS_HPP
#define KCD2S_VOICE_VOICE_TENANTS_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "kcd2s/voice/control_link.hpp"
#include "kcd2s/voice/voice_server.hpp"

namespace kcd2s::voice {

class VoiceTenants {
 public:
  using Clock = VoiceServer::Clock;
  using Sender = VoiceServer::Sender;

  // A control link that is gone freezes the last positions, and mutes after this long.
  static constexpr std::uint64_t kLinkLostMuteMs = 5000;
  // Each tenant's sessions: this many, so a frame's session tells its tenant at once.
  static constexpr std::uint32_t kSessionsPerTenant = 1u << 20;

  VoiceTenants(RelayConfig config, Sender sender, Clock clock, std::uint16_t voice_port);

  // A link said Hello. Answers with the Welcome to send back. The same id takes up its tenant
  // again.
  ControlMessage link_opened(const std::string& server_id);
  // A message of that tenant's link. A Talking answer comes from take_talking.
  void on_control(const std::string& server_id, const ControlMessage& message);
  void link_lost(const std::string& server_id);
  // From the UDP thread: a datagram goes to the tenant its session says.
  void offer(const Address& from, const std::uint8_t* data, std::size_t size);
  // Every 100 ms: mutes tenants whose link has been gone too long, and runs their relays.
  void tick();
  std::vector<ControlMessage> take_talking(const std::string& server_id);
  std::size_t tenants() const;
  bool enabled(const std::string& server_id) const;
  std::size_t players(const std::string& server_id) const;

 private:
  struct Joined {
    std::uint32_t session = 0;
    Key client_key{};
  };
  struct Tenant {
    std::unique_ptr<VoiceServer> relay;
    std::uint32_t base = 0;
    bool wanted_enabled = true;
    std::optional<std::uint64_t> lost_at;
    bool muted_by_loss = false;
    // Who is joined, with their client's key. A Join with the same key again (a link that came
    // back) changes nothing: the player's counters go on, and no nonce comes twice.
    std::map<std::uint32_t, Joined> joined;
    // Named since the link opened. Synced drops the others: a game server restarted meanwhile.
    std::set<std::uint32_t> named;
  };

  RelayConfig config_;
  Sender sender_;
  Clock clock_;
  std::uint16_t voice_port_ = 0;
  KeyPair keys_;
  std::uint32_t next_base_ = kSessionsPerTenant;
  mutable std::mutex mutex_;
  std::map<std::string, Tenant> tenants_;
  std::map<std::uint32_t, VoiceServer*> by_base_;
};

}  // namespace kcd2s::voice

#endif
