#include "kcd2s/voice/voice_server.hpp"

#include <chrono>
#include <utility>

namespace kcd2s::voice {

VoiceServer::VoiceServer(RelayConfig config, Sender sender, Clock clock)
    : sender_(std::move(sender)), clock_(std::move(clock)), relay_(config) {
}

VoiceServer::~VoiceServer() {
  stop();
}

void VoiceServer::start() {
  if (!thread_.joinable()) {
    thread_ = std::thread([this] { run(); });
  }
}

void VoiceServer::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void VoiceServer::offer(const Address& from, const std::uint8_t* data, std::size_t size) {
  VoiceHeader header;
  // Up only, and of a possible length: nothing else is worth a place in the queue.
  if (!read_voice_header(data, size, header) || header.tag != kVoiceUpTag) {
    std::lock_guard<std::mutex> lock(mutex_);
    counters_.refused += 1;
    return;
  }
  Inbound inbound{from, std::vector<std::uint8_t>(data, data + size)};
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (inbound_.size() >= kQueueLimit) {
      // The oldest goes: late voice is worth less than new voice.
      inbound_.pop_front();
      counters_.queue_drops += 1;
    }
    inbound_.push_back(std::move(inbound));
  }
  wake_.notify_one();
}

void VoiceServer::post(std::function<void()> command) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    commands_.push_back(std::move(command));
  }
  wake_.notify_one();
}

void VoiceServer::add_player(core::EntityId player, std::uint32_t session, const Key& session_key) {
  post([this, player, session, session_key] {
    const auto old = peers_.find(player);
    if (old != peers_.end()) {
      sessions_.erase(old->second.session);
    }
    Session& entry = sessions_[session];
    entry = Session{};
    entry.player = player;
    entry.key = session_key;
    Peer peer;
    peer.session = session;
    peer.key = session_key;
    peers_[player] = peer;
    relay_.add_player(player);
  });
}

void VoiceServer::remove_player(core::EntityId player) {
  post([this, player] {
    const auto found = peers_.find(player);
    if (found == peers_.end()) {
      return;
    }
    if (found->second.talking) {
      std::lock_guard<std::mutex> lock(mutex_);
      talk_changes_.push_back(TalkChange{player, false});
    }
    sessions_.erase(found->second.session);
    peers_.erase(found);
    relay_.remove_player(player);
  });
}

void VoiceServer::set_position(core::EntityId player, const core::Vec3& position,
                               std::uint32_t world) {
  post([this, player, position, world] { relay_.set_position(player, position, world); });
}

void VoiceServer::set_enabled(bool enabled) {
  post([this, enabled] { relay_.set_enabled(enabled); });
}

void VoiceServer::set_tier_range(Tier tier, float meters) {
  post([this, tier, meters] { relay_.set_tier_range(tier, meters); });
}

void VoiceServer::set_tier(core::EntityId player, Tier tier) {
  post([this, player, tier] { relay_.set_tier(player, tier); });
}

void VoiceServer::set_muted(core::EntityId player, bool muted) {
  post([this, player, muted] { relay_.set_muted(player, muted); });
}

void VoiceServer::set_deaf(core::EntityId player, bool deaf) {
  post([this, player, deaf] { relay_.set_deaf(player, deaf); });
}

void VoiceServer::set_listener_mute(core::EntityId listener, core::EntityId talker, bool muted) {
  post([this, listener, talker, muted] { relay_.set_listener_mute(listener, talker, muted); });
}

std::vector<TalkChange> VoiceServer::take_talk_changes() {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::exchange(talk_changes_, {});
}

VoiceCounters VoiceServer::counters() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return counters_;
}

void VoiceServer::handle(const Inbound& inbound, std::uint64_t now) {
  VoiceHeader header;
  bool accepted = read_voice_header(inbound.data.data(), inbound.data.size(), header);
  const auto session = accepted ? sessions_.find(header.id) : sessions_.end();
  accepted =
      accepted && session != sessions_.end() && session->second.replay.may_accept(header.counter);
  accepted = accepted && open_voice(inbound.data.data(), inbound.data.size(), header,
                                    session->second.key, payload_);
  if (!accepted) {
    std::lock_guard<std::mutex> lock(mutex_);
    counters_.refused += 1;
    return;
  }
  session->second.replay.mark(header.counter);
  const core::EntityId talker = session->second.player;
  Peer& speaker = peers_[talker];
  // A datagram only an admitted player could seal: this is where their voice comes from now.
  speaker.address = inbound.from;
  speaker.address_known = true;
  // An empty frame only says where the player can be reached (their game's hello).
  if (payload_.empty()) {
    return;
  }
  speaker.last_frame_ms = now;
  if (!speaker.talking && relay_.enabled()) {
    speaker.talking = true;
    std::lock_guard<std::mutex> lock(mutex_);
    talk_changes_.push_back(TalkChange{talker, true});
  }
  relay_.route(talker, header.loudness, now, listeners_);
  std::uint64_t sent = 0;
  for (const core::EntityId id : listeners_) {
    const auto listener = peers_.find(id);
    if (listener == peers_.end() || !listener->second.address_known) {
      continue;
    }
    VoiceHeader down;
    down.tag = kVoiceDownTag;
    down.id = talker;
    down.counter = ++listener->second.down_counter;
    down.loudness = header.loudness;
    if (seal_voice(down, listener->second.key, payload_.data(), payload_.size(), sealed_)) {
      sender_(listener->second.address, sealed_);
      ++out_this_second_;
      ++sent;
    }
  }
  std::lock_guard<std::mutex> lock(mutex_);
  counters_.frames_in += 1;
  counters_.frames_out += sent;
}

void VoiceServer::end_silent_talkers(std::uint64_t now) {
  for (auto& [id, peer] : peers_) {
    if (peer.talking && now - peer.last_frame_ms > kTalkEndMs) {
      peer.talking = false;
      std::lock_guard<std::mutex> lock(mutex_);
      talk_changes_.push_back(TalkChange{id, false});
    }
  }
}

std::size_t VoiceServer::pump() {
  std::vector<std::function<void()>> commands;
  std::deque<Inbound> inbound;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    commands.swap(commands_);
    inbound.swap(inbound_);
  }
  for (const auto& command : commands) {
    command();
  }
  const std::uint64_t now = clock_();
  if (inbound.size() >= kBehindDatagrams) {
    behind_ = true;
  } else if (inbound.size() <= kCaughtUpDatagrams) {
    behind_ = false;
  }
  if (now - second_started_ms_ >= 1000) {
    if (out_this_second_ > out_budget_) {
      over_budget_ = true;
    } else if (out_this_second_ <= out_budget_ / 2) {
      over_budget_ = false;
    }
    out_this_second_ = 0;
    second_started_ms_ = now;
  }
  std::size_t speakers = kMaxSpeakers;
  if (behind_) {
    speakers = kSpeakersWhenBehind;
  } else if (over_budget_) {
    speakers = kSpeakersOverBudget;
  }
  relay_.set_speakers_allowed(speakers);
  for (const Inbound& item : inbound) {
    handle(item, now);
  }
  end_silent_talkers(now);
  return inbound.size();
}

void VoiceServer::run() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stopping_) {
    wake_.wait_for(lock, std::chrono::milliseconds(100),
                   [this] { return stopping_ || !inbound_.empty() || !commands_.empty(); });
    if (stopping_) {
      break;
    }
    lock.unlock();
    pump();
    lock.lock();
  }
}

}  // namespace kcd2s::voice
