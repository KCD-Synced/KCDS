#include "kcd2s/voice/voice_tenants.hpp"

#include <utility>

namespace kcd2s::voice {

VoiceTenants::VoiceTenants(RelayConfig config, Sender sender, Clock clock, std::uint16_t voice_port)
    : config_(config),
      sender_(std::move(sender)),
      clock_(std::move(clock)),
      voice_port_(voice_port),
      keys_(make_key_pair()) {
}

ControlMessage VoiceTenants::link_opened(const std::string& server_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = tenants_.find(server_id);
  if (found == tenants_.end()) {
    Tenant tenant;
    tenant.relay = std::make_unique<VoiceServer>(config_, sender_, clock_);
    tenant.relay->start();
    tenant.base = next_base_;
    next_base_ += kSessionsPerTenant;
    by_base_[tenant.base] = tenant.relay.get();
    found = tenants_.emplace(server_id, std::move(tenant)).first;
  }
  Tenant& tenant = found->second;
  tenant.lost_at.reset();
  tenant.named.clear();
  if (tenant.muted_by_loss) {
    tenant.muted_by_loss = false;
    tenant.relay->set_enabled(tenant.wanted_enabled);
  }
  ControlMessage welcome;
  welcome.kind = ControlKind::Welcome;
  welcome.key = keys_.public_key;
  welcome.port = voice_port_;
  welcome.session = tenant.base;
  return welcome;
}

void VoiceTenants::on_control(const std::string& server_id, const ControlMessage& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = tenants_.find(server_id);
  if (found == tenants_.end()) {
    return;
  }
  Tenant& tenant = found->second;
  VoiceServer& relay = *tenant.relay;
  switch (message.kind) {
    case ControlKind::Join: {
      // Only inside this tenant's own range: one game server cannot take another's sessions.
      if (message.session < tenant.base || message.session >= tenant.base + kSessionsPerTenant) {
        return;
      }
      tenant.named.insert(message.player);
      const auto known = tenant.joined.find(message.player);
      if (known != tenant.joined.end() && known->second.session == message.session &&
          known->second.client_key == message.key) {
        return;
      }
      Key session_key{};
      if (derive_session_key(keys_.secret_key, message.key, message.key, keys_.public_key,
                             session_key)) {
        relay.add_player(message.player, message.session, session_key);
        tenant.joined[message.player] = Joined{message.session, message.key};
      }
      break;
    }
    case ControlKind::Leave:
      relay.remove_player(message.player);
      tenant.joined.erase(message.player);
      tenant.named.erase(message.player);
      break;
    case ControlKind::Synced:
      for (auto it = tenant.joined.begin(); it != tenant.joined.end();) {
        if (tenant.named.count(it->first) != 0) {
          ++it;
        } else {
          relay.remove_player(it->first);
          it = tenant.joined.erase(it);
        }
      }
      break;
    case ControlKind::Positions:
      for (const ControlPosition& entry : message.positions) {
        relay.set_position(entry.player, entry.position, entry.world);
      }
      break;
    case ControlKind::Enabled:
      tenant.wanted_enabled = message.flag;
      relay.set_enabled(message.flag);
      break;
    case ControlKind::Range:
      relay.set_tier_range(static_cast<Tier>(message.tier), message.meters);
      break;
    case ControlKind::Tier:
      relay.set_tier(message.player, static_cast<Tier>(message.tier));
      break;
    case ControlKind::Muted:
      relay.set_muted(message.player, message.flag);
      break;
    case ControlKind::Deaf:
      relay.set_deaf(message.player, message.flag);
      break;
    case ControlKind::ListenerMute:
      relay.set_listener_mute(message.player, message.other, message.flag);
      break;
    default:
      break;
  }
}

void VoiceTenants::link_lost(const std::string& server_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = tenants_.find(server_id);
  if (found != tenants_.end()) {
    found->second.lost_at = clock_();
  }
}

void VoiceTenants::offer(const Address& from, const std::uint8_t* data, std::size_t size) {
  VoiceHeader header;
  if (!read_voice_header(data, size, header)) {
    return;
  }
  const std::uint32_t base = header.id - header.id % kSessionsPerTenant;
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = by_base_.find(base);
  if (found != by_base_.end()) {
    found->second->offer(from, data, size);
  }
}

void VoiceTenants::tick() {
  const std::uint64_t now = clock_();
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& [id, tenant] : tenants_) {
    (void)id;
    if (tenant.lost_at && !tenant.muted_by_loss && now - *tenant.lost_at >= kLinkLostMuteMs) {
      tenant.muted_by_loss = true;
      tenant.relay->set_enabled(false);
    }
  }
}

std::vector<ControlMessage> VoiceTenants::take_talking(const std::string& server_id) {
  std::vector<ControlMessage> out;
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = tenants_.find(server_id);
  if (found == tenants_.end()) {
    return out;
  }
  for (const TalkChange& change : found->second.relay->take_talk_changes()) {
    ControlMessage message;
    message.kind = ControlKind::Talking;
    message.player = change.player;
    message.flag = change.talking;
    out.push_back(message);
  }
  return out;
}

std::size_t VoiceTenants::tenants() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return tenants_.size();
}

std::size_t VoiceTenants::players(const std::string& server_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = tenants_.find(server_id);
  return found == tenants_.end() ? 0 : found->second.joined.size();
}

bool VoiceTenants::enabled(const std::string& server_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = tenants_.find(server_id);
  return found != tenants_.end() && !found->second.muted_by_loss && found->second.wanted_enabled;
}

}  // namespace kcd2s::voice
