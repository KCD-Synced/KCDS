// What the game server tells its voice relay, wherever that relay runs: in this process
// (VoiceServer) or on a standalone voice server (RemoteRelay). None of these waits on voice work.
#ifndef KCD2S_VOICE_RELAY_PORT_HPP
#define KCD2S_VOICE_RELAY_PORT_HPP

#include <cstdint>
#include <vector>

#include "kcd2s/core/math.hpp"
#include "kcd2s/core/ids.hpp"
#include "kcd2s/voice/relay.hpp"

namespace kcd2s::voice {

struct TalkChange {
  core::EntityId player = core::kInvalidEntity;
  bool talking = false;
};

class RelayPort {
 public:
  virtual ~RelayPort() = default;
  virtual void remove_player(core::EntityId player) = 0;
  virtual void set_position(core::EntityId player, const core::Vec3& position,
                            std::uint32_t world) = 0;
  virtual void set_enabled(bool enabled) = 0;
  virtual void set_tier_range(Tier tier, float meters) = 0;
  virtual void set_tier(core::EntityId player, Tier tier) = 0;
  virtual void set_muted(core::EntityId player, bool muted) = 0;
  virtual void set_deaf(core::EntityId player, bool deaf) = 0;
  virtual void set_listener_mute(core::EntityId listener, core::EntityId talker, bool muted) = 0;
  virtual std::vector<TalkChange> take_talk_changes() = 0;
};

}  // namespace kcd2s::voice

#endif
