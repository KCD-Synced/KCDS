#pragma once

#include <cstdint>
#include <optional>

namespace kcds::runtime::knockout {

// Engine-thread identity only. These body addresses never enter a network message.
struct ReplicaWakeScope {
  std::uint32_t revision = 0;
  std::uint8_t epoch = 0;
  std::uintptr_t actor = 0;
  std::uintptr_t soul = 0;
  std::uintptr_t model = 0;
  bool operator==(const ReplicaWakeScope&) const = default;
};

struct ReplicaWakeObservation {
  bool native_ko = false;
  bool scope7 = false;
  bool scope9 = false;
};

// Tracks one actual owner down/up transition. Native gates, pointer validation and the
// original wake call remain in the engine adapter; a void native return is not completion.
class ReplicaWake {
 public:
  bool bind(const ReplicaWakeScope& scope) {
    if (!scope.revision || !scope.actor || !scope.soul || !scope.model) {
      clear();
      return false;
    }
    if (!scope_ || *scope_ != scope) {
      clear();
      scope_ = scope;
    }
    return true;
  }

  // A movement epoch retires old network work, but does not replace this native body's
  // fallen action. The adapter validates the current body and epoch before transferring
  // that lifecycle. Every observation using the previous epoch is still rejected.
  bool continue_body(const ReplicaWakeScope& scope) {
    if (scope_ && scope.revision == scope_->revision && scope.actor == scope_->actor &&
        scope.soul == scope_->soul && scope.model == scope_->model) {
      scope_ = scope;
      return true;
    }
    return bind(scope);
  }

  void clear() {
    scope_.reset();
    stage_ = Stage::Idle;
    owner_known_ = false;
    owner_down_ = false;
    native_known_ = false;
    native_ = {};
    observed_getup_ = false;
    observed_fallen_ = false;
    observed_exit_ = false;
    attempted_ = false;
    completion_consumed_ = false;
  }

  bool observe_owner(const ReplicaWakeScope& scope, bool down) {
    if (!matches(scope)) {
      return false;
    }
    if (down && (!owner_known_ || !owner_down_)) {
      stage_ = Stage::Down;
      observed_getup_ = false;
      observed_fallen_ = false;
      observed_exit_ = false;
      attempted_ = false;
      completion_consumed_ = false;
    } else if (!down && owner_known_ && owner_down_) {
      stage_ = observed_getup_ ? Stage::GettingUp : Stage::Pending;
      attempted_ = false;
    }
    owner_known_ = true;
    owner_down_ = down;
    settle();
    return true;
  }

  bool observe_native(const ReplicaWakeScope& scope, const ReplicaWakeObservation& value) {
    if (!matches(scope)) {
      return false;
    }
    const bool entered = value.scope9 && (!native_known_ || !native_.scope9);
    const bool exited = native_known_ && native_.scope9 && !value.scope9;
    native_ = value;
    native_known_ = true;
    if (stage_ == Stage::Unmanaged && value.scope7 && !owner_down_) {
      stage_ = Stage::Pending;
    }
    if (stage_ == Stage::Down || stage_ == Stage::Pending || stage_ == Stage::Requested ||
        stage_ == Stage::GettingUp) {
      observed_fallen_ = observed_fallen_ || value.scope7;
      if (entered) {
        observed_getup_ = true;
        observed_exit_ = false;
        if (!owner_down_) {
          stage_ = Stage::GettingUp;
        }
      } else if (observed_getup_ && exited) {
        observed_exit_ = true;
      }
    }
    // Owner ragdoll replication can put a body into Sleep without a native fallen
    // action. There is then no scope-7 action for the original wake request to retire.
    // Yield to that existing presentation owner; this is not native GetUp completion.
    if (stage_ == Stage::Pending && !observed_fallen_ && !observed_getup_ && !value.native_ko &&
        !value.scope7 && !value.scope9) {
      stage_ = Stage::Unmanaged;
    }
    settle();
    return true;
  }

  // Call immediately before the original native wake, after its current gates are validated.
  // The state changes first so a reentrant update cannot request the same wake twice.
  bool request(const ReplicaWakeScope& scope, bool original_guards_ready) {
    if (!matches(scope) || stage_ != Stage::Pending || !native_known_ || native_.native_ko ||
        !native_.scope7 || native_.scope9 || !original_guards_ready) {
      return false;
    }
    attempted_ = true;
    stage_ = Stage::Requested;
    return true;
  }

  bool owns_body() const {
    return stage_ == Stage::Pending || stage_ == Stage::Requested || stage_ == Stage::GettingUp;
  }

  bool pending() const {
    return stage_ == Stage::Pending;
  }

  bool requested() const {
    return attempted_;
  }

  bool complete() const {
    return stage_ == Stage::Complete;
  }

  bool take_completion(const ReplicaWakeScope& scope) {
    if (!matches(scope) || !complete() || completion_consumed_) {
      return false;
    }
    completion_consumed_ = true;
    return true;
  }

 private:
  enum class Stage { Idle, Down, Pending, Requested, GettingUp, Complete, Unmanaged };

  bool matches(const ReplicaWakeScope& scope) const {
    return scope_ && *scope_ == scope;
  }

  void settle() {
    if (stage_ == Stage::GettingUp && !owner_down_ && observed_exit_ && native_known_ &&
        !native_.native_ko && !native_.scope7 && !native_.scope9) {
      stage_ = Stage::Complete;
    }
  }

  std::optional<ReplicaWakeScope> scope_;
  Stage stage_ = Stage::Idle;
  bool owner_known_ = false;
  bool owner_down_ = false;
  bool native_known_ = false;
  ReplicaWakeObservation native_;
  bool observed_getup_ = false;
  bool observed_fallen_ = false;
  bool observed_exit_ = false;
  bool attempted_ = false;
  bool completion_consumed_ = false;
};

}  // namespace kcds::runtime::knockout
