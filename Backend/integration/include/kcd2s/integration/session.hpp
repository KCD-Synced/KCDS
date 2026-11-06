// Capstone multiplayer session (definition-of-done core loop, headless). Ties the whole stack
// together so the prototype's "multiple players visible on a server" goal is exercised end to end:
//
//   - N clients join a server-authoritative Server over SimLinks.
//   - The server spawns one Player avatar per client, grants that client ownership, and replicates
//     every avatar, so each client's Mirror sees every other player.
//   - A replicated "players" SharedState (key player:<pid> -> display name) syncs the roster to all
//     clients, firing change callbacks.
//   - Chat is broadcast over NetBus: a client says something -> server -> every client.
//   - On leave the avatar is despawned (clean DESTRUCT) and the roster entry removed; both
//     replicate to the remaining clients.
//
// The server is always authoritative (avatars, health, roster are server-owned; only a player's own
// position is owner-authored upstream). Everything runs on an advancing clock. This is a reusable
// scenario a test drives step by step.
#ifndef KCD2S_INTEGRATION_SESSION_HPP
#define KCD2S_INTEGRATION_SESSION_HPP

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "kcd2s/core/ids.hpp"
#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/netbus.hpp"
#include "kcd2s/integration/server.hpp"
#include "kcd2s/integration/shared_state_sync.hpp"
#include "kcd2s/replication/schema.hpp"
#include "kcd2s/sdk/shared_state.hpp"
#include "kcd2s/transport/connection.hpp"
#include "kcd2s/transport/datagram.hpp"

namespace kcd2s::integration {

class Session {
 public:
  Session();

  // A client joins with a display name; returns its PeerId. Spawns + owns its avatar, adds roster.
  PeerId join(const std::string& name);
  // A client leaves: despawn its avatar, remove its roster entry, drop it from the server.
  void leave(PeerId pid);
  // The owning client authors its avatar position upstream.
  void move(PeerId pid, const replication::Vec3& pos);
  // The client broadcasts a chat line (client -> server -> all clients).
  void say(PeerId pid, const std::string& msg);
  // Advance `n` server+client ticks on the shared clock.
  void tick(int n = 1);

  // --- introspection for tests ---
  const replication::Mirror& mirror(PeerId pid) const;
  core::EntityId avatar_of(PeerId pid) const;
  std::size_t players_visible_to(PeerId pid) const;  // Player avatars in that client's mirror
  std::size_t roster_size(PeerId pid) const;         // entries in that client's "players" bag
  bool roster_has(PeerId pid, PeerId who) const;     // does pid's roster list player:<who>?
  const std::vector<std::string>& chat_log(PeerId pid) const;
  const std::vector<std::string>& roster_changes(PeerId pid) const;
  std::size_t member_count() const {
    return members_.size();
  }
  const replication::SchemaRegistry& schema() const {
    return schema_;
  }
  core::TypeId player_type() const {
    return player_t_;
  }

 private:
  struct Member {
    PeerId pid = 0;
    std::string name;
    core::EntityId avatar = 0;
    std::unique_ptr<transport::SimLink> rs, rc;  // replication link pair (server/client)
    std::unique_ptr<ClientPeer> peer;
    std::unique_ptr<transport::SimLink> bs, bc;  // bus link pair (server/client)
    std::unique_ptr<transport::Connection> sbus_conn, cbus_conn;
    std::unique_ptr<NetBus> sbus, cbus;
    sdk::SharedStateStore client_store;
    std::unique_ptr<ClientSharedStates> client_bags;
    std::vector<std::string> chat_log;
    std::vector<std::string> roster_changes;
  };

  void broadcast_chat(const sdk::Value::Array& args);
  Member& member(PeerId pid) const;

  replication::SchemaRegistry schema_;
  core::TypeId player_t_ = 0;
  Server server_;
  sdk::SharedStateStore server_store_;
  ServerSharedStates server_bags_;
  std::map<PeerId, std::unique_ptr<Member>> members_;
  double now_ = 0.0;
  int spawn_counter_ = 0;
};

}  // namespace kcd2s::integration

#endif  // KCD2S_INTEGRATION_SESSION_HPP
