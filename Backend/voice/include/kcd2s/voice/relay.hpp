// The voice relay's rules: who hears a talker's frame. No sockets and no threads
// here: the host owns those, and one thread calls all of this. Nothing decodes or stores audio.
#ifndef KCD2S_VOICE_RELAY_HPP
#define KCD2S_VOICE_RELAY_HPP

#include <array>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "kcd2s/core/ids.hpp"
#include "kcd2s/core/math.hpp"
#include "kcd2s/replication/interest.hpp"

namespace kcd2s::voice {

enum class Tier : std::uint8_t { Whisper = 0, Normal = 1, Shout = 2 };

struct RelayConfig {
  // The range of each tier in metres, by Tier.
  std::array<float, 3> ranges{2.0f, 10.0f, 30.0f};
  float grid_cell_meters = 50.0f;
  // A talker silent this long gives up their place at a listener.
  std::uint32_t speaker_timeout_ms = 400;
  // How much better a new talker's score must be than the lowest to take its place.
  float take_over_margin = 1.25f;
  // How many talkers a listener hears now, at most kMaxSpeakers: fewer while the relay is behind.
  std::size_t speakers_allowed = 8;
};

// How many talkers one listener hears at once.
constexpr std::size_t kMaxSpeakers = 8;

// The places of one listener's talkers.
class SpeakerSelector {
 public:
  // Whether this talker is heard now. Score 0 to 1, higher is better. Takes a free or expired
  // place, or the lowest one when the score beats it by the margin.
  bool admit(core::EntityId talker, float score, std::uint64_t now_ms, const RelayConfig& config);
  void forget(core::EntityId talker);
  std::size_t active(std::uint64_t now_ms, const RelayConfig& config) const;

 private:
  struct Place {
    core::EntityId talker = core::kInvalidEntity;
    float score = 0.0f;
    std::uint64_t heard_ms = 0;
  };
  std::array<Place, kMaxSpeakers> places_{};
};

class Relay {
 public:
  explicit Relay(RelayConfig config = {});

  const RelayConfig& config() const {
    return config_;
  }

  // The whole server: while off, no frame goes anywhere.
  void set_enabled(bool enabled) {
    enabled_ = enabled;
  }
  bool enabled() const {
    return enabled_;
  }
  void set_tier_range(Tier tier, float meters);
  void set_speakers_allowed(std::size_t speakers);
  std::size_t speakers_allowed() const {
    return config_.speakers_allowed;
  }

  void add_player(core::EntityId player);
  void remove_player(core::EntityId player);
  bool has_player(core::EntityId player) const {
    return players_.count(player) != 0;
  }
  void set_position(core::EntityId player, const core::Vec3& position, std::uint32_t world);
  void set_tier(core::EntityId player, Tier tier);
  // The server silenced this talker for everyone.
  void set_muted(core::EntityId player, bool muted);
  // This player hears nobody.
  void set_deaf(core::EntityId player, bool deaf);
  // This listener alone does not hear that talker; other listeners are not touched.
  void set_listener_mute(core::EntityId listener, core::EntityId talker, bool muted);

  // The listeners of a talker's frame, in `out` (cleared first, its memory is reused). Loudness 0
  // to 255 comes from the packet header; a lie moves only the talker's place, never their range.
  void route(core::EntityId talker, std::uint8_t loudness, std::uint64_t now_ms,
             std::vector<core::EntityId>& out);

 private:
  struct Player;
  struct Candidate {
    core::EntityId id = core::kInvalidEntity;
    Player* player = nullptr;
    float closeness = 0.0f;
  };
  static constexpr std::uint64_t kCandidatesMs = 100;
  void refresh_candidates(core::EntityId talker, Player& source, float range, std::uint64_t now_ms);

  struct Player {
    core::Vec3 position{};
    std::uint32_t world = 0;
    bool placed = false;
    Tier tier = Tier::Normal;
    bool muted = false;
    bool deaf = false;
    std::unordered_set<core::EntityId> muted_talkers;
    SpeakerSelector speakers;
    // Who this talker reaches, kept for kCandidatesMs: positions come 10 times a second, frames 50.
    std::vector<Candidate> candidates;
    std::uint64_t candidates_at = 0;
    std::uint64_t candidates_revision = 0;
  };

  RelayConfig config_;
  bool enabled_ = true;
  replication::InterestGrid grid_;
  // Anything but a move changes this, and every kept list is made anew.
  std::uint64_t revision_ = 1;
  std::unordered_map<core::EntityId, Player> players_;
};

}  // namespace kcd2s::voice

#endif
