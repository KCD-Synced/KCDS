// A body's animation hooks identify its skeleton; movement hooks identify its animated
// character. Keep both identities together so a movement request never scans every replica.
#pragma once

#include <cstddef>
#include <unordered_map>

namespace kcds::runtime::replicas {

template <class State>
class MotionIndex {
 public:
  using Entries = std::unordered_map<void*, State>;
  using const_iterator = typename Entries::const_iterator;

  void assign(void* skeleton, const State& state) {
    const auto current = entries_.find(skeleton);
    if (current != entries_.end() && current->second.animated != state.animated) {
      const auto previous = animated_.find(current->second.animated);
      if (previous != animated_.end() && previous->second == skeleton) {
        animated_.erase(previous);
      }
    }
    if (state.animated) {
      const auto previous = animated_.find(state.animated);
      if (previous != animated_.end() && previous->second != skeleton) {
        entries_.erase(previous->second);
      }
      animated_[state.animated] = skeleton;
    }
    entries_.insert_or_assign(skeleton, state);
  }

  const State* by_animated(void* animated) const {
    const auto found = animated_.find(animated);
    if (found == animated_.end()) {
      return nullptr;
    }
    const auto entry = entries_.find(found->second);
    return entry == entries_.end() ? nullptr : &entry->second;
  }

  const_iterator find(void* skeleton) const {
    return entries_.find(skeleton);
  }

  const_iterator end() const {
    return entries_.end();
  }

  void erase(void* skeleton) {
    const auto found = entries_.find(skeleton);
    if (found == entries_.end()) {
      return;
    }
    const auto animated = animated_.find(found->second.animated);
    if (animated != animated_.end() && animated->second == skeleton) {
      animated_.erase(animated);
    }
    entries_.erase(found);
  }

  void clear() {
    animated_.clear();
    entries_.clear();
  }

  std::size_t size() const {
    return entries_.size();
  }

 private:
  Entries entries_;
  std::unordered_map<void*, void*> animated_;
};

}  // namespace kcds::runtime::replicas
