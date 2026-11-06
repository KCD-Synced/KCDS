// Shared state: replicated key->Value maps.
//
// A SharedState is a named, scoped (global / entity:<id> / player:<id>) map from string key to the
// canonical sdk::Value. Resources read/write bags to share arbitrary state; the server is
// authoritative: a bag declares who may author its writes (BagAuthority), and the store rejects a
// client attempt to write a server-authored bag. Changes replicate as a compact DELTA (only dirty
// keys + tombstones) over the reliable Events channel, encoded with sdk::marshal so values stay
// language-independent. The receiving side applies the delta and fires per-key change callbacks
// with (old, new) so scripts can react.
//
// This module is transport-agnostic: encode_delta() produces bytes, apply_delta() consumes them.
// The Events-channel plumbing (NetBus) carries them. All decoding treats input as untrusted
// (bounded key count + key length, graceful rejection).
#ifndef KCD2S_SDK_SHARED_STATE_HPP
#define KCD2S_SDK_SHARED_STATE_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "kcd2s/sdk/value.hpp"

namespace kcd2s::sdk {

// Who is allowed to author writes to a bag. The server may always write; a client may author only
// Client-authority bags. This mirrors the replication Authority (Server vs Owner) model.
enum class BagAuthority : std::uint8_t { Server = 0, Client = 1 };

// Defensive bounds for untrusted delta decoding.
inline constexpr std::uint32_t kMaxBagKeys = 4096;   // keys touched in a single delta
inline constexpr std::uint32_t kMaxBagKeyLen = 256;  // bytes in a key

// One replicated map. Tracks which keys changed since the last encode_delta(), so a delta carries
// only what moved.
class SharedState {
 public:
  // key, old value (null if the key is new), new value (null if the key was removed).
  using ChangeFn = std::function<void(const std::string&, const Value&, const Value&)>;

  explicit SharedState(BagAuthority auth = BagAuthority::Server) : auth_(auth) {
  }

  BagAuthority authority() const {
    return auth_;
  }

  const Value* get(const std::string& key) const {
    auto it = values_.find(key);
    return it == values_.end() ? nullptr : &it->second;
  }
  std::size_t size() const {
    return values_.size();
  }
  const std::map<std::string, Value>& entries() const {
    return values_;
  }

  // Author a write. Returns true if the stored value actually changed (and marks the key dirty).
  // A no-op write (same value) neither changes state nor dirties the key.
  bool set(const std::string& key, Value v);

  // Remove a key. Returns true if it existed (and records a dirty tombstone so the removal
  // replicates).
  bool erase(const std::string& key);

  // Dirty tracking (what a delta would carry).
  bool dirty() const {
    return !dirty_.empty() || !removed_.empty();
  }
  void clear_dirty() {
    dirty_.clear();
    removed_.clear();
  }

  // Serialize the dirty set + tombstones into a delta (does NOT clear dirty; call clear_dirty()
  // after a successful send). A full snapshot is encode_full().
  std::vector<std::uint8_t> encode_delta() const;
  std::vector<std::uint8_t> encode_full() const;

  // Apply a delta/snapshot produced by encode_delta()/encode_full(). Fires `on_change` for every
  // key whose value changed (old -> new; new is null for a removal). Returns false on malformed
  // input (and leaves the bag unchanged past the point of failure is avoided: it validates lengths
  // before mutating each entry). Applying is the authoritative (server->client) path and is not
  // gated by authority.
  bool apply_delta(const std::uint8_t* data, std::size_t len, const ChangeFn& on_change = {});
  bool apply_delta(const std::vector<std::uint8_t>& buf, const ChangeFn& on_change = {}) {
    return apply_delta(buf.data(), buf.size(), on_change);
  }

 private:
  std::map<std::string, Value> values_;
  std::map<std::string, bool> dirty_;    // key -> present (ordered for deterministic deltas)
  std::map<std::string, bool> removed_;  // tombstones since last clear_dirty
  BagAuthority auth_;
};

// A collection of named bags (the scope is encoded in the name, e.g. "global", "entity:42",
// "player:3"). The store enforces write authority: author_write() succeeds only if the writer is
// the server or the bag is Client-authority.
class SharedStateStore {
 public:
  // Get or create a bag. New bags default to Server authority.
  SharedState& bag(const std::string& name, BagAuthority auth = BagAuthority::Server);
  const SharedState* find(const std::string& name) const;
  bool has(const std::string& name) const {
    return bags_.count(name) != 0;
  }
  std::size_t size() const {
    return bags_.size();
  }

  // Author a write respecting authority. `is_server` is true on the server. Returns false (and
  // writes nothing) when a client tries to author a Server-authority bag.
  bool author_write(const std::string& name, const std::string& key, Value v, bool is_server);

  // Iterate all bags (for a replicator that broadcasts dirty deltas / full snapshots).
  std::unordered_map<std::string, SharedState>& bags() {
    return bags_;
  }
  const std::unordered_map<std::string, SharedState>& bags() const {
    return bags_;
  }

 private:
  std::unordered_map<std::string, SharedState> bags_;
};

}  // namespace kcd2s::sdk

#endif  // KCD2S_SDK_SHARED_STATE_HPP
