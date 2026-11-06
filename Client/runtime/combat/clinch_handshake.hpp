#pragma once

#include <cstdint>
#include <unordered_map>

namespace kcds::runtime::combat {

// The clinch handshake between the local player's client and the client of the other player
// The game starts a clinch through (kind, initiator, other): it marks
// the initiator's clinch component (movement + 1008, + 24) as the one holding the initiative and
// gives both guards their GuardSyncMovement. A new clinch comes from the approach,
// which each client decides from where it sees the two players; a replica lags its owner, so a new
// clinch between the local player and a replica starts only when the other client has started it
// too: both games show the same clinch with the same initiator. A clinch going on after one of its
// transitions or pairs is the game's own (a guard start, continues it from the
// clinch state those left, in the same frame); it takes no handshake (answered a round
// trip later, the guard start had already built a plain guard, which ended the clinch).

// The native clinch starts the wire carries (CombatAction::Clinch value).
inline constexpr int kGuardClinch = 0;     // a guard start, the clinch going on
inline constexpr int kApproachClinch = 1;  // the approach, a new clinch

// The local game asks again for an unanswered or refused clinch after this long, while it keeps
// wanting it.
inline constexpr std::uint64_t kClinchRequestRepeatMs = 500;
// How long the initiator takes the other client's accept of its request.
inline constexpr std::uint64_t kClinchAcceptWindowMs = 1000;

enum class ClinchAnswer {
  // Both asked at the same time and the local request wins: the lower player id initiates.
  Ignore,
  // Start the clinch now and accept it.
  Start,
  // Not startable here (not locked on each other, not both in their guards, or this client does
  // not see the approach): the requester is told at once, nothing waits.
  Refuse,
};

// The local client's side of the handshake, per other player. Engine thread.
class ClinchHandshake {
 public:
  // The local game wants a clinch the local player initiates against `other`: true when the
  // request goes out now (not again within kClinchRequestRepeatMs of the last one).
  bool request(std::uint32_t other, int code, std::uint64_t now) {
    auto own = requested_.find(other);
    if (own != requested_.end() && now - own->second.at <= kClinchRequestRepeatMs) {
      return false;
    }
    requested_[other] = {code, now, false};
    return true;
  }

  // `source` asks the local client to start the clinch `source` initiates; `startable` is whether
  // the game here would start it now.
  ClinchAnswer answer(std::uint32_t local, std::uint32_t source, bool startable,
                      std::uint64_t now) {
    auto own = requested_.find(source);
    if (own != requested_.end() && !own->second.answered) {
      const bool fresh = now - own->second.at <= kClinchAcceptWindowMs;
      if (fresh && local < source) {
        return ClinchAnswer::Ignore;
      }
      // The other's request wins: no accept is waited for any more.
      own->second.answered = true;
    }
    return startable ? ClinchAnswer::Start : ClinchAnswer::Refuse;
  }

  // `source` started the clinch the local player asked for: true when it answers the local
  // request (same kind, within the accept window, not refused or overtaken). Answered once.
  bool accepted(std::uint32_t source, int code, std::uint64_t now) {
    auto own = requested_.find(source);
    if (own == requested_.end() || own->second.answered ||
        now - own->second.at > kClinchAcceptWindowMs || own->second.code != code) {
      return false;
    }
    own->second.answered = true;
    return true;
  }

  // `source` refused the clinch the local player asked for: true when that answers the local
  // request. A late accept for it is then ignored; the game asks again only after the repeat time.
  bool refused(std::uint32_t source, int code) {
    auto own = requested_.find(source);
    if (own == requested_.end() || own->second.answered || own->second.code != code) {
      return false;
    }
    own->second.answered = true;
    return true;
  }

  // The player's replica is gone.
  void forget(std::uint32_t player) {
    requested_.erase(player);
  }

  // The world changed.
  void reset() {
    requested_.clear();
  }

 private:
  struct Entry {
    int code = 0;
    std::uint64_t at = 0;
    bool answered = false;
  };
  // The local player's last request to each player.
  std::unordered_map<std::uint32_t, Entry> requested_;
};

}  // namespace kcds::runtime::combat
