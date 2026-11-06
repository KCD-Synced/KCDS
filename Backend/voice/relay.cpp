#include "kcd2s/voice/relay.hpp"

#include <algorithm>
#include <cmath>

namespace kcd2s::voice {
namespace {

replication::InterestConfig grid_config(const RelayConfig& config) {
  replication::InterestConfig grid;
  grid.cell_size = config.grid_cell_meters;
  grid.up = replication::UpAxis::Z;
  return grid;
}

float distance_between(const core::Vec3& a, const core::Vec3& b) {
  const float dx = a.x - b.x;
  const float dy = a.y - b.y;
  const float dz = a.z - b.z;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

bool SpeakerSelector::admit(core::EntityId talker, float score, std::uint64_t now_ms,
                            const RelayConfig& config) {
  Place* free_place = nullptr;
  Place* lowest = nullptr;
  std::size_t index = 0;
  for (Place& place : places_) {
    const bool allowed = index++ < config.speakers_allowed;
    if (place.talker == talker && !allowed) {
      // The limit came down past this place: its talker competes again for one that is allowed.
      place.talker = core::kInvalidEntity;
      continue;
    }
    if (place.talker == talker) {
      place.score = score;
      place.heard_ms = now_ms;
      return true;
    }
    const bool expired =
        place.talker == core::kInvalidEntity || now_ms - place.heard_ms > config.speaker_timeout_ms;
    if (expired) {
      if (!free_place && allowed) {
        free_place = &place;
      }
    } else if (!lowest || place.score < lowest->score) {
      lowest = &place;
    }
  }
  Place* taken = free_place;
  if (!taken && lowest && score > lowest->score * config.take_over_margin) {
    taken = lowest;
  }
  if (!taken) {
    return false;
  }
  taken->talker = talker;
  taken->score = score;
  taken->heard_ms = now_ms;
  return true;
}

void SpeakerSelector::forget(core::EntityId talker) {
  for (Place& place : places_) {
    if (place.talker == talker) {
      place = Place{};
    }
  }
}

std::size_t SpeakerSelector::active(std::uint64_t now_ms, const RelayConfig& config) const {
  std::size_t count = 0;
  for (const Place& place : places_) {
    if (place.talker != core::kInvalidEntity &&
        now_ms - place.heard_ms <= config.speaker_timeout_ms) {
      ++count;
    }
  }
  return count;
}

Relay::Relay(RelayConfig config) : config_(config), grid_(grid_config(config)) {
}

void Relay::set_tier_range(Tier tier, float meters) {
  ++revision_;
  if (std::isfinite(meters) && meters >= 0.0f) {
    config_.ranges[static_cast<std::size_t>(tier)] = meters;
  }
}

void Relay::set_speakers_allowed(std::size_t speakers) {
  config_.speakers_allowed = std::clamp(speakers, std::size_t{1}, kMaxSpeakers);
}

void Relay::add_player(core::EntityId player) {
  ++revision_;
  players_.try_emplace(player);
}

void Relay::remove_player(core::EntityId player) {
  ++revision_;
  if (players_.erase(player) == 0) {
    return;
  }
  grid_.remove(player);
  for (auto& [id, other] : players_) {
    other.muted_talkers.erase(player);
    other.speakers.forget(player);
  }
}

void Relay::set_position(core::EntityId player, const core::Vec3& position, std::uint32_t world) {
  const auto found = players_.find(player);
  if (found == players_.end()) {
    return;
  }
  found->second.position = position;
  // A change of world counts at once: a list of last moment must not cross worlds.
  if (found->second.world != world || !found->second.placed) {
    ++revision_;
  }
  found->second.world = world;
  found->second.placed = true;
  grid_.set_position(player, position);
}

void Relay::set_tier(core::EntityId player, Tier tier) {
  ++revision_;
  const auto found = players_.find(player);
  if (found != players_.end()) {
    found->second.tier = tier;
  }
}

void Relay::set_muted(core::EntityId player, bool muted) {
  ++revision_;
  const auto found = players_.find(player);
  if (found != players_.end()) {
    found->second.muted = muted;
  }
}

void Relay::set_deaf(core::EntityId player, bool deaf) {
  ++revision_;
  const auto found = players_.find(player);
  if (found != players_.end()) {
    found->second.deaf = deaf;
  }
}

void Relay::set_listener_mute(core::EntityId listener, core::EntityId talker, bool muted) {
  ++revision_;
  const auto found = players_.find(listener);
  if (found == players_.end()) {
    return;
  }
  if (muted) {
    found->second.muted_talkers.insert(talker);
    found->second.speakers.forget(talker);
  } else {
    found->second.muted_talkers.erase(talker);
  }
}

void Relay::refresh_candidates(core::EntityId talker, Player& source, float range,
                               std::uint64_t now_ms) {
  source.candidates.clear();
  source.candidates_at = now_ms;
  source.candidates_revision = revision_;
  for (const core::EntityId id : grid_.query(source.position, range)) {
    if (id == talker) {
      continue;
    }
    const auto hearing = players_.find(id);
    if (hearing == players_.end()) {
      continue;
    }
    Player& listener = hearing->second;
    if (listener.deaf || listener.world != source.world ||
        listener.muted_talkers.count(talker) != 0) {
      continue;
    }
    const float distance = distance_between(source.position, listener.position);
    if (distance <= range) {
      source.candidates.push_back(Candidate{id, &listener, 1.0f - distance / range});
    }
  }
}

void Relay::route(core::EntityId talker, std::uint8_t loudness, std::uint64_t now_ms,
                  std::vector<core::EntityId>& out) {
  out.clear();
  const auto speaking = players_.find(talker);
  if (!enabled_ || speaking == players_.end() || !speaking->second.placed ||
      speaking->second.muted) {
    return;
  }
  Player& source = speaking->second;
  const float range = config_.ranges[static_cast<std::size_t>(source.tier)];
  if (range <= 0.0f) {
    return;
  }
  if (source.candidates_revision != revision_ || now_ms - source.candidates_at >= kCandidatesMs) {
    refresh_candidates(talker, source, range, now_ms);
  }
  const float loud = static_cast<float>(loudness) / 255.0f;
  // Even a whispering talker right here scores above zero: 0.2 of it is closeness alone.
  const float level = 0.2f + 0.8f * loud;
  for (const Candidate& candidate : source.candidates) {
    if (candidate.player->speakers.admit(talker, candidate.closeness * level, now_ms, config_)) {
      out.push_back(candidate.id);
    }
  }
}

}  // namespace kcd2s::voice
