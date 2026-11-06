// State-bag replication over the reliable Events channel. Ties the SDK
// state-bag store (Backend/sdk/shared_state) to the network via NetBus, keeping the server
// authoritative.
//
//   server:  ServerSharedStates(store) + add_client(bus) per connected client.
//            set(bag,key,v) authors a server write; broadcast_dirty() ships each dirty bag's delta
//            to every client bus then clears dirty; a freshly added client is sent a full snapshot.
//            Inbound client proposals (ClientSharedStates::propose) are applied through the store's
//            authority guard — a write to a Server-authority bag from a client is rejected.
//   client:  ClientSharedStates(store, bus, onChange) applies inbound deltas/snapshots into its
//   store
//            and fires onChange(bag,key,old,new) per changed key. propose(bag,key,v) asks the
//            server to write (only honored for Client-authority bags).
//
// Wire: two NetBus events. "sb:delta" = { Str bag, Bool full, Bytes payload } (payload is a
// SharedState delta or full snapshot). "sb:write" = { Str bag, Str key, Bytes value } (a client
// proposal; value is a marshalled sdk::Value). All inbound is validated (tags + decode) and dropped
// if malformed.
#ifndef KCD2S_INTEGRATION_SHARED_STATE_SYNC_HPP
#define KCD2S_INTEGRATION_SHARED_STATE_SYNC_HPP

#include <functional>
#include <string>
#include <vector>

#include "kcd2s/integration/netbus.hpp"
#include "kcd2s/sdk/shared_state.hpp"
#include "kcd2s/sdk/value.hpp"

namespace kcd2s::integration {

inline constexpr char kSbDeltaEvent[] = "sb:delta";
inline constexpr char kSbWriteEvent[] = "sb:write";

// Server replicator: owns no store, references one. Broadcasts to every registered client bus.
class ServerSharedStates {
 public:
  explicit ServerSharedStates(sdk::SharedStateStore& store) : store_(store) {
  }

  // Register a client's NetBus. Immediately queues a full snapshot of all current bags to it, and
  // subscribes to that client's write proposals (authority-checked).
  void add_client(NetBus& bus);

  // Author a server-side write (always allowed) and mark it for the next broadcast.
  void set(const std::string& bag, const std::string& key, sdk::Value v,
           sdk::BagAuthority auth = sdk::BagAuthority::Server);

  // Remove a key server-side; the tombstone replicates on the next broadcast_dirty().
  void erase(const std::string& bag, const std::string& key);

  // Stop replicating to a client's bus (call before its NetBus is destroyed, e.g. on disconnect).
  void remove_client(NetBus& bus);

  // Emit each dirty bag's delta to every client bus, then clear the dirty sets.
  void broadcast_dirty();

 private:
  void send_snapshot_to(NetBus& bus);

  sdk::SharedStateStore& store_;
  std::vector<NetBus*> buses_;
};

// Client replicator: applies inbound deltas/snapshots into its store, firing a change callback.
class ClientSharedStates {
 public:
  // bag, key, old value (null if new), new value (null if removed).
  using ChangeFn = std::function<void(const std::string&, const std::string&, const sdk::Value&,
                                      const sdk::Value&)>;

  ClientSharedStates(sdk::SharedStateStore& store, NetBus& bus, ChangeFn on_change = {});

  // Ask the server to write a key (honored only for Client-authority bags, server-side).
  void propose(const std::string& bag, const std::string& key, sdk::Value v);

 private:
  sdk::SharedStateStore& store_;
  NetBus& bus_;
  ChangeFn on_change_;
};

}  // namespace kcd2s::integration

#endif  // KCD2S_INTEGRATION_SHARED_STATE_SYNC_HPP
