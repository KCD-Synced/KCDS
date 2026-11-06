// Native RemoveEntity may defer destruction. A network departure does not release the
// character's allocation until its original native identity has actually disappeared.
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace kcds::runtime::replicas {

class RetirementFence {
 public:
  // Session reset does not prove native destruction. Only a different nonnull entity
  // system owns a different ID table; a null or repeated system preserves pending bodies.
  void observe_system(const void* system) {
    if (system && system != system_) {
      active_.clear();
      pending_.clear();
      system_ = system;
    }
  }

  void created(std::uint32_t id, const void* body) {
    if (!id || !body) {
      return;
    }
    const auto retired = pending_.find(id);
    if (retired != pending_.end()) {
      if (retired->second == body) {
        return;
      }
      pending_.erase(retired);
    }
    active_[id] = body;
  }

  void retire(std::uint32_t id, const void* body) {
    if (id && body) {
      const auto active = active_.find(id);
      if (active != active_.end()) {
        if (active->second != body) {
          return;
        }
        active_.erase(active);
      }
      pending_[id] = body;
    }
  }

  template <class Lookup>
  void refresh(const Lookup& lookup) {
    for (auto it = active_.begin(); it != active_.end();) {
      if (lookup(it->first) != it->second) {
        it = active_.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it = pending_.begin(); it != pending_.end();) {
      if (lookup(it->first) != it->second) {
        it = pending_.erase(it);
      } else {
        ++it;
      }
    }
  }

  bool can_spawn(std::size_t admitted) const {
    return active_.size() + pending_.size() < admitted;
  }

  std::size_t active() const {
    return active_.size();
  }

  std::size_t pending() const {
    return pending_.size();
  }

 private:
  const void* system_ = nullptr;
  std::unordered_map<std::uint32_t, const void*> active_;
  std::unordered_map<std::uint32_t, const void*> pending_;
};

}  // namespace kcds::runtime::replicas
