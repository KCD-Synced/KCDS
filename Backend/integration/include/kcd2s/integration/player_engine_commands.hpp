#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "kcd2s/core/ids.hpp"
#include "kcd2s/integration/player_stats.hpp"

namespace kcd2s::integration {

// Engine-published scalar identity. Incarnation changes when the native body/session changes,
// never merely because that same body is knocked out, revived or given a forced-state epoch.
struct PlayerEngineScope {
  std::uint32_t revision = 0;
  core::EntityId owner = 0;
  std::uint64_t incarnation = 0;

  friend bool operator==(const PlayerEngineScope& a, const PlayerEngineScope& b) {
    return a.revision == b.revision && a.owner == b.owner && a.incarnation == b.incarnation;
  }
};

struct PlayerReviveCommand {
  core::EntityId player = 0;
};

// A resource knocks the player out (KNOCK_OUT): the game's own unconsciousness, the health set to
// `health`.
struct PlayerKnockOutCommand {
  float health = 1.0f;
};

struct PlayerEngineCommand {
  PlayerEngineScope scope;
  std::uint64_t sequence = 0;
  std::variant<StatRequest, PlayerReviveCommand, PlayerKnockOutCommand> payload;
};

enum class PlayerEngineCommandStatus {
  Command,
  Empty,
  Unbound,
  ScopeMismatch,
  Overflow,
};

struct PlayerEngineCommandPoll {
  PlayerEngineCommandStatus status = PlayerEngineCommandStatus::Empty;
  std::optional<PlayerEngineCommand> command;
  // Lifetime counters survive resets, so a consumer can log losses even after a scope changes.
  std::uint64_t overflows = 0;
  std::uint64_t discarded_commands = 0;
};

struct PlayerEngineCommandDiagnostics {
  std::size_t pending_commands = 0;
  std::uint64_t overflows = 0;
  std::uint64_t discarded_commands = 0;
  bool poisoned = false;
};

// Caller provides synchronization. Contains no native pointers, health policy or wake timing.
// Before the first body bind, receipts are staged in their original Control0 order. Legacy
// consumers remove their own kinds from this same stream; they are disabled after ordered bind.
class PlayerEngineCommandQueue {
 public:
  static constexpr std::size_t kCapacity = 64;

  void reset_world(std::uint32_t revision) {
    discard_pending();
    scope_.reset();
    revision_ = revision;
    adopt_staged_ = revision != 0;
    poisoned_ = false;
  }

  void invalidate_body() {
    discard_pending();
    scope_.reset();
    adopt_staged_ = false;
    poisoned_ = false;
  }

  bool bind(const PlayerEngineScope& scope) {
    if (scope.revision == 0 || scope.revision != revision_ || scope.owner == 0 ||
        scope.incarnation == 0) {
      return false;
    }
    if (scope_ && *scope_ == scope) {
      return true;  // Idempotent: this must not erase an overflow or pending commands.
    }
    if (scope.incarnation <= last_incarnation_) {
      return false;
    }
    if (adopt_staged_) {
      for (auto it = commands_.begin(); it != commands_.end();) {
        const auto* revive = std::get_if<PlayerReviveCommand>(&it->payload);
        if (it->scope.revision != scope.revision || (revive && revive->player != scope.owner)) {
          ++discarded_commands_;
          it = commands_.erase(it);
        } else {
          it->scope = scope;
          ++it;
        }
      }
    } else {
      discard_pending();
      poisoned_ = false;
    }
    scope_ = scope;
    last_incarnation_ = scope.incarnation;
    adopt_staged_ = false;
    ordered_ = true;
    return true;
  }

  // A SET_STAT or ADD_STAT request, in receipt order with the revives.
  bool push_stat(std::uint32_t revision, const StatRequest& request) {
    if (!stat_request_valid(request)) {
      return false;
    }
    return push(revision, request);
  }

  bool push_revive(std::uint32_t revision, core::EntityId player) {
    if (player == 0 || (scope_ && player != scope_->owner)) {
      ++discarded_commands_;
      return false;
    }
    return push(revision, PlayerReviveCommand{player});
  }

  bool push_knock_out(std::uint32_t revision, float health) {
    if (!(health > 0.0f) || health > 1.0e9f) {
      ++discarded_commands_;
      return false;
    }
    return push(revision, PlayerKnockOutCommand{health});
  }

  // Non-consuming admission check, including an empty stream while native wake is pending.
  bool current(const PlayerEngineScope& expected) const {
    return scope_ && *scope_ == expected && !poisoned_;
  }

  PlayerEngineCommandPoll take(const PlayerEngineScope& expected) {
    if (!scope_) {
      return status(PlayerEngineCommandStatus::Unbound);
    }
    if (!(*scope_ == expected)) {
      return status(PlayerEngineCommandStatus::ScopeMismatch);
    }
    if (poisoned_) {
      return status(PlayerEngineCommandStatus::Overflow);
    }
    if (commands_.empty()) {
      return status(PlayerEngineCommandStatus::Empty);
    }
    auto result = status(PlayerEngineCommandStatus::Command);
    result.command = std::move(commands_.front());
    commands_.pop_front();
    return result;
  }

  PlayerEngineCommandPoll status(PlayerEngineCommandStatus value) const {
    return {value, std::nullopt, overflows_, discarded_commands_};
  }

  PlayerEngineCommandDiagnostics diagnostics() const {
    return {commands_.size(), overflows_, discarded_commands_, poisoned_};
  }

  std::vector<StatRequest> take_legacy_stats() {
    std::vector<StatRequest> result;
    if (ordered_ || poisoned_) {
      return result;
    }
    for (auto it = commands_.begin(); it != commands_.end();) {
      if (const auto* stat = std::get_if<StatRequest>(&it->payload)) {
        result.push_back(*stat);
        it = commands_.erase(it);
      } else {
        ++it;
      }
    }
    return result;
  }

  std::size_t take_legacy_revives() {
    if (ordered_ || poisoned_) {
      return 0;
    }
    std::size_t count = 0;
    for (auto it = commands_.begin(); it != commands_.end();) {
      if (std::holds_alternative<PlayerReviveCommand>(it->payload)) {
        ++count;
        it = commands_.erase(it);
      } else {
        ++it;
      }
    }
    return count;
  }

 private:
  template <typename Payload>
  bool push(std::uint32_t revision, const Payload& payload) {
    if (revision == 0 || revision != revision_ || (!scope_ && !adopt_staged_)) {
      ++discarded_commands_;
      return false;
    }
    if (poisoned_) {
      ++discarded_commands_;
      return false;
    }
    if (commands_.size() == kCapacity ||
        next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
      ++overflows_;
      poisoned_ = true;
      discard_pending();
      ++discarded_commands_;
      return false;
    }
    const PlayerEngineScope receipt_scope = scope_.value_or(PlayerEngineScope{revision, 0, 0});
    commands_.push_back({receipt_scope, next_sequence_++, payload});
    return true;
  }

  void discard_pending() {
    discarded_commands_ += commands_.size();
    commands_.clear();
  }

  std::deque<PlayerEngineCommand> commands_;
  std::optional<PlayerEngineScope> scope_;
  std::uint32_t revision_ = 0;
  std::uint64_t last_incarnation_ = 0;
  std::uint64_t next_sequence_ = 1;
  std::uint64_t overflows_ = 0;
  std::uint64_t discarded_commands_ = 0;
  bool adopt_staged_ = false;
  bool ordered_ = false;
  bool poisoned_ = false;
};

}  // namespace kcd2s::integration
