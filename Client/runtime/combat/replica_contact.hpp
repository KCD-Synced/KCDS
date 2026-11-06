#pragma once

#include <cstdint>
#include <optional>

namespace kcds::runtime::combat {

// Local engine-thread identity. These addresses never travel over the wire.
struct ReplicaContactScope {
  std::uint32_t revision = 0;
  std::uint8_t epoch = 0;
  std::uintptr_t actor = 0;
  std::uintptr_t movement = 0;
  bool operator==(const ReplicaContactScope&) const = default;
};

class ReplicaContact {
 public:
  bool announce(const ReplicaContactScope& scope, std::uint64_t generation, unsigned kind) {
    if (!scope.revision || !scope.actor || !scope.movement || !generation ||
        (kind != 3 && kind != 4)) {
      return false;
    }
    if (scope_ && *scope_ == scope && generation <= generation_) {
      return false;
    }
    scope_ = scope;
    generation_ = generation;
    kind_ = kind;
    action_ = 0;
    pending_ = true;
    started_ = false;
    consumed_ = false;
    return true;
  }

  bool construct(const ReplicaContactScope& scope, std::uintptr_t action, unsigned kind) {
    if (!scope_ || *scope_ != scope || !pending_ || !action || kind != kind_) {
      return false;
    }
    pending_ = false;
    action_ = action;
    return true;
  }

  bool start(const ReplicaContactScope& scope, std::uintptr_t action, unsigned kind,
             bool during_input = true) {
    if (pending_ && during_input) {
      construct(scope, action, kind);
    }
    if (!scope_ || *scope_ != scope || !action_ || action_ != action || kind != kind_) {
      return false;
    }
    started_ = true;
    return true;
  }

  void exit(std::uintptr_t action) {
    if (action_ == action) {
      action_ = 0;
      started_ = false;
    }
  }

  // A refused or unavailable original input must not leave its marker waiting
  // to bind a later swing whose marker was dropped by preview backpressure.
  void finish_input() {
    pending_ = false;
  }

  // Consume once, only while the same original primary action is still active.
  bool consume(const ReplicaContactScope& scope, std::uint64_t generation, std::uintptr_t action,
               unsigned kind) {
    if (!scope_ || *scope_ != scope || consumed_ || !started_ || !action_ || action != action_ ||
        kind != kind_ || generation != generation_) {
      return false;
    }
    consumed_ = true;
    return true;
  }

 private:
  std::optional<ReplicaContactScope> scope_;
  std::uint64_t generation_ = 0;
  unsigned kind_ = 0;
  std::uintptr_t action_ = 0;
  bool pending_ = false;
  bool started_ = false;
  bool consumed_ = false;
};

}  // namespace kcds::runtime::combat
