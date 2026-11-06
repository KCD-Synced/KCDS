#pragma once
// Last value reported to the resource host per key. The host reports player and horse
// state only when it differs from what the resource host already holds, so an idle server sends
// nothing and the bridge cost scales with change instead of with the number of players. Portable
// so the bookkeeping is covered by the Linux unit tests.
#include <cstddef>
#include <unordered_map>

namespace kcd2s::local::resources {

template <typename Key, typename Value>
class ReportCache {
 public:
  // True when `value` must be sent: the key was never reported, was forgotten, or its last
  // reported value differs. The value is remembered as reported.
  bool changed(const Key& key, const Value& value) {
    auto found = last_.find(key);
    if (found == last_.end()) {
      last_.emplace(key, value);
      return true;
    }
    if (found->second == value) {
      return false;
    }
    found->second = value;
    return true;
  }

  // The next changed() for this key reports again, for example after the resource host changed
  // its local copy without a native confirmation, or after the entity left.
  void forget(const Key& key) {
    last_.erase(key);
  }

  void clear() {
    last_.clear();
  }

  std::size_t size() const {
    return last_.size();
  }

 private:
  std::unordered_map<Key, Value> last_;
};

}  // namespace kcd2s::local::resources
