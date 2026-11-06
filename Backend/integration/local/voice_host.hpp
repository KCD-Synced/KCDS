#pragma once
// The dedicated server's side of voice chat: owns the relay and its session keys,
// and keeps the relay told who plays where. Runs on the game tick; the relay itself has its own
// thread. With use_external the relay is a standalone voice server instead, told over a link that
// may come and go: each time it comes, the relay is told everything again.
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "kcd2s/integration/voice_messages.hpp"
#include "kcd2s/voice/remote_relay.hpp"
#include "kcd2s/voice/voice_server.hpp"

namespace kcd2s::local {

class VoiceHost {
 public:
  using Sender = voice::VoiceServer::Sender;
  using SendState = std::function<bool(core::EntityId, const integration::VoiceState&)>;
  using SendMuffle = std::function<bool(core::EntityId, core::EntityId, std::uint8_t)>;

  VoiceHost(Sender sender, SendState send_state)
      : send_state_(std::move(send_state)),
        keys_(voice::make_key_pair()),
        embedded_(voice::RelayConfig{}, std::move(sender), [] {
          const auto now = std::chrono::steady_clock::now().time_since_epoch();
          return static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
        }) {
    relay_ = &embedded_;
  }

  // Before the first player: the relay is a standalone voice server. Until its first Welcome,
  // players are told voice is off.
  void use_external(voice::RemoteRelayOptions options) {
    remote_ = std::make_unique<voice::RemoteRelay>(std::move(options));
    relay_ = remote_.get();
    remote_->start();
  }
  bool external() const {
    return remote_ != nullptr;
  }

  void set_muffle_sender(SendMuffle send) {
    send_muffle_ = std::move(send);
  }

  void start_thread() {
    if (!remote_) {
      embedded_.start();
      threaded_ = true;
    }
  }
  // From the socket's reader thread.
  void offer(std::uint32_t ip, std::uint16_t port, const std::uint8_t* data, std::size_t size) {
    if (!remote_) {
      embedded_.offer(voice::Address{ip, port}, data, size);
    }
  }
  // Each tick. For a host without the thread what arrived is handled here; with a standalone voice
  // server this takes up a link that has just come.
  void pump_inline() {
    if (remote_) {
      take_up_link();
    } else if (!threaded_) {
      embedded_.pump();
    }
  }

  // Each 100 ms: a player is ready to hear about voice. Position and world of a player who has
  // given their key, sent on when they moved.
  void player_ready(core::EntityId player) {
    Player& entry = players_[player];
    if (entry.number == 0) {
      entry.number = ++next_session_;
    }
    tell(player, entry);
    // A player who comes to hear voice learns who is muffled, once.
    if (!entry.muffles_told && send_muffle_) {
      bool all = true;
      for (const auto& [talker, amount] : muffles_) {
        all = send_muffle_(player, talker, amount) && all;
      }
      entry.muffles_told = all;
    }
  }
  void player_at(core::EntityId player, const core::Vec3& position, std::uint32_t world) {
    const auto found = players_.find(player);
    if (found == players_.end() || !found->second.keyed) {
      return;
    }
    Player& entry = found->second;
    const float dx = position.x - entry.position.x;
    const float dy = position.y - entry.position.y;
    const float dz = position.z - entry.position.z;
    if (entry.placed && entry.world == world && dx * dx + dy * dy + dz * dz < kMoveMeters2) {
      return;
    }
    entry.placed = true;
    entry.position = position;
    entry.world = world;
    relay_->set_position(player, position, world);
  }
  void player_gone(core::EntityId player) {
    muffles_.erase(player);
    if (players_.erase(player) != 0) {
      relay_->remove_player(player);
      forget_listener_mutes(player);
    }
  }
  template <class StillHere>
  void forget_gone_players(StillHere still_here) {
    for (auto it = players_.begin(); it != players_.end();) {
      if (still_here(it->first)) {
        ++it;
      } else {
        relay_->remove_player(it->first);
        forget_listener_mutes(it->first);
        it = players_.erase(it);
      }
    }
  }
  void player_key(core::EntityId player, const std::array<std::uint8_t, 32>& key) {
    const auto found = players_.find(player);
    if (found == players_.end()) {
      return;
    }
    voice::Key client_key{};
    std::copy(key.begin(), key.end(), client_key.begin());
    Player& entry = found->second;
    if (remote_) {
      if (!linked_) {
        return;
      }
      remote_->join(player, session_of(entry), client_key);
    } else {
      voice::Key session_key{};
      if (!voice::derive_session_key(keys_.secret_key, client_key, client_key, keys_.public_key,
                                     session_key)) {
        return;
      }
      embedded_.add_player(player, session_of(entry), session_key);
    }
    entry.client_key = client_key;
    tell_relay_about(player, entry);
    entry.keyed = true;
    entry.placed = false;
  }

  void set_enabled(bool enabled) {
    enabled_ = enabled;
    relay_->set_enabled(enabled);
    tell_everyone();
  }
  bool enabled() const {
    return enabled_;
  }
  // Whether the players' games draw their voice indicator (Voice.setIndicator).
  void set_indicator(bool shown) {
    indicator_ = shown;
    tell_everyone();
  }
  void set_range(voice::Tier tier, float meters) {
    ranges_[static_cast<std::size_t>(tier)] = meters;
    relay_->set_tier_range(tier, meters);
    tell_everyone();
  }
  void set_tier(core::EntityId player, std::uint8_t tier) {
    Player& entry = players_[player];
    entry.tier = tier;
    relay_->set_tier(player, static_cast<voice::Tier>(tier));
    tell(player, entry);
  }
  void set_muted(core::EntityId player, bool muted) {
    Player& entry = players_[player];
    entry.muted = muted;
    relay_->set_muted(player, muted);
    tell(player, entry);
  }
  void set_deaf(core::EntityId player, bool deaf) {
    Player& entry = players_[player];
    entry.deaf = deaf;
    relay_->set_deaf(player, deaf);
    tell(player, entry);
  }
  // How muffled a talker sounds to everyone, 0 to 255 (a gag). Only how it sounds: they are heard.
  void set_muffle(core::EntityId talker, std::uint8_t amount) {
    if (amount == 0) {
      muffles_.erase(talker);
    } else {
      muffles_[talker] = amount;
    }
    if (!send_muffle_) {
      return;
    }
    for (const auto& [id, entry] : players_) {
      if (entry.number != 0) {
        send_muffle_(id, talker, amount);
      }
    }
  }

  void set_listener_mute(core::EntityId listener, core::EntityId talker, bool muted) {
    relay_->set_listener_mute(listener, talker, muted);
    if (muted) {
      listener_mutes_.insert({listener, talker});
    } else {
      listener_mutes_.erase({listener, talker});
    }
  }
  std::vector<voice::TalkChange> take_talk_changes() {
    return relay_->take_talk_changes();
  }
  voice::VoiceCounters counters() const {
    return embedded_.counters();
  }

 private:
  // The relay need not hear of a move smaller than this (0.5 m squared): ranges are in metres.
  static constexpr float kMoveMeters2 = 0.25f;

  struct Player {
    // The player's number here; their session is the relay's base plus this.
    std::uint32_t number = 0;
    voice::Key client_key{};
    std::uint8_t tier = 1;
    bool muted = false;
    bool deaf = false;
    bool keyed = false;
    bool placed = false;
    core::Vec3 position{};
    std::uint32_t world = 0;
    integration::VoiceState told{};
    bool told_any = false;
    bool muffles_told = false;
  };

  void tell(core::EntityId player, Player& entry) {
    if (entry.number == 0) {
      return;
    }
    integration::VoiceState state;
    state.enabled = enabled_ && (!remote_ || linked_);
    state.tier = entry.tier;
    state.muted = entry.muted;
    state.deaf = entry.deaf;
    state.session = session_of(entry);
    std::copy(keys_.public_key.begin(), keys_.public_key.end(), state.server_key.begin());
    state.voice_ip = endpoint_.ip;
    state.voice_port = endpoint_.port;
    state.ranges = ranges_;
    state.indicator = indicator_;
    if (entry.told_any && entry.told == state) {
      return;
    }
    if (send_state_(player, state)) {
      entry.told = state;
      entry.told_any = true;
    }
  }
  void tell_everyone() {
    for (auto& [id, entry] : players_) {
      tell(id, entry);
    }
  }

  std::uint32_t session_of(const Player& entry) const {
    return session_base_ + entry.number;
  }
  void tell_relay_about(core::EntityId player, const Player& entry) {
    relay_->set_tier(player, static_cast<voice::Tier>(entry.tier));
    relay_->set_muted(player, entry.muted);
    relay_->set_deaf(player, entry.deaf);
  }
  void forget_listener_mutes(core::EntityId player) {
    for (auto it = listener_mutes_.begin(); it != listener_mutes_.end();) {
      if (it->first == player || it->second == player) {
        it = listener_mutes_.erase(it);
      } else {
        ++it;
      }
    }
  }
  // A new link. A voice server that restarted has a new key, so players make a new session with it
  // when they are told; one that only lost the link keeps theirs, and hears them named again.
  void take_up_link() {
    const std::optional<voice::RemoteWelcome> welcome = remote_->take_welcome();
    if (!welcome) {
      return;
    }
    const bool same_server = linked_before_ && keys_.public_key == welcome->public_key &&
                             session_base_ == welcome->session_base;
    keys_.public_key = welcome->public_key;
    session_base_ = welcome->session_base;
    endpoint_ = welcome->endpoint;
    linked_ = true;
    linked_before_ = true;
    remote_->set_enabled(enabled_);
    for (std::size_t tier = 0; tier < ranges_.size(); ++tier) {
      remote_->set_tier_range(static_cast<voice::Tier>(tier), ranges_[tier]);
    }
    for (auto& [id, entry] : players_) {
      if (!same_server) {
        entry.keyed = false;
      }
      if (entry.keyed) {
        remote_->join(id, session_of(entry), entry.client_key);
        tell_relay_about(id, entry);
        entry.placed = false;
      }
    }
    for (const auto& [listener, talker] : listener_mutes_) {
      remote_->set_listener_mute(listener, talker, true);
    }
    remote_->mark_synced();
    tell_everyone();
  }

  SendState send_state_;
  SendMuffle send_muffle_;
  std::map<core::EntityId, std::uint8_t> muffles_;
  voice::KeyPair keys_;
  voice::VoiceServer embedded_;
  std::unique_ptr<voice::RemoteRelay> remote_;
  voice::RelayPort* relay_ = nullptr;
  bool linked_ = false;
  bool linked_before_ = false;
  std::uint32_t session_base_ = 0;
  voice::Address endpoint_{};
  std::set<std::pair<core::EntityId, core::EntityId>> listener_mutes_;
  bool threaded_ = false;
  bool enabled_ = true;
  bool indicator_ = true;
  std::array<float, 3> ranges_{2.0f, 10.0f, 30.0f};
  std::uint32_t next_session_ = 0;
  std::unordered_map<core::EntityId, Player> players_;
};

}  // namespace kcd2s::local
