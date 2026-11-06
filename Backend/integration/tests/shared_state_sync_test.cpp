#include "kcd2s/integration/shared_state_sync.hpp"

#include <memory>
#include <string>
#include <vector>

#include "kcd2s/integration/netbus.hpp"
#include "kcd2s/sdk/shared_state.hpp"
#include "kcd2s/sdk/value.hpp"
#include "kcd2s/testkit.hpp"
#include "kcd2s/transport/connection.hpp"
#include "kcd2s/transport/datagram.hpp"
#include "kcd2s/wire/packet.hpp"

using kcd2s::integration::ClientSharedStates;
using kcd2s::integration::NetBus;
using kcd2s::integration::ServerSharedStates;
using kcd2s::sdk::BagAuthority;
using kcd2s::sdk::SharedStateStore;
using kcd2s::sdk::Value;
using kcd2s::transport::Connection;
using kcd2s::transport::make_sim_pair;
using kcd2s::transport::SimLink;
using kcd2s::wire::Channel;

namespace {

struct Change {
  std::string bag, key;
  Value old_v, new_v;
};

// One connected client: its links, the server-side + client-side NetBus, a client store/replicator,
// and a recorded change log.
struct Client {
  std::unique_ptr<SimLink> s, c;
  std::unique_ptr<Connection> s_conn, c_conn;
  std::unique_ptr<NetBus> s_bus, c_bus;
  SharedStateStore store;
  std::unique_ptr<ClientSharedStates> repl;
  std::vector<Change> changes;
};

const std::uint8_t kEv = static_cast<std::uint8_t>(Channel::Events);

std::unique_ptr<Client> join(ServerSharedStates& server) {
  auto cl = std::make_unique<Client>();
  auto pr = make_sim_pair({}, {});
  cl->s = std::move(pr.first);
  cl->c = std::move(pr.second);
  cl->s_conn = std::make_unique<Connection>(cl->s.get());
  cl->c_conn = std::make_unique<Connection>(cl->c.get());
  cl->s_bus = std::make_unique<NetBus>(*cl->s_conn);
  cl->c_bus = std::make_unique<NetBus>(*cl->c_conn);
  Client* raw = cl.get();
  cl->repl = std::make_unique<ClientSharedStates>(
      cl->store, *cl->c_bus,
      [raw](const std::string& bag, const std::string& key, const Value& o, const Value& n) {
        raw->changes.push_back({bag, key, o, n});
      });
  server.add_client(*cl->s_bus);  // queues a full snapshot to this client
  return cl;
}

void pump(std::vector<std::unique_ptr<Client>>& clients, int n) {
  static double now = 0.0;
  for (int i = 0; i < n; ++i) {
    now += 1.0 / 60.0;
    for (auto& cl : clients) {
      cl->s_conn->flush(now);
      cl->c_conn->flush(now);
      for (auto& m : cl->c_conn->receive(now))
        if (m.channel == kEv)
          cl->c_bus->feed(m.bytes);
      for (auto& m : cl->s_conn->receive(now))
        if (m.channel == kEv)
          cl->s_bus->feed(m.bytes);
    }
  }
}

}  // namespace

KCD2S_TEST(shared_state_sync, server_write_converges_to_clients) {
  SharedStateStore server_store;
  ServerSharedStates server(server_store);

  std::vector<std::unique_ptr<Client>> clients;
  clients.push_back(join(server));
  clients.push_back(join(server));

  server.set("global", "mission", Value(std::string("rattay")));
  server.broadcast_dirty();
  pump(clients, 4);

  for (auto& cl : clients) {
    const auto* b = cl->store.find("global");
    KCD2S_CHECK(b != nullptr);
    KCD2S_CHECK_EQ(b->get("mission")->as_str(), std::string("rattay"));
    // change callback fired null -> "rattay"
    bool saw = false;
    for (auto& ch : cl->changes)
      if (ch.bag == "global" && ch.key == "mission") {
        saw = true;
        KCD2S_CHECK(ch.old_v.is_null());
        KCD2S_CHECK_EQ(ch.new_v.as_str(), std::string("rattay"));
      }
    KCD2S_CHECK(saw);
  }

  // An update replicates old -> new.
  for (auto& cl : clients)
    cl->changes.clear();
  server.set("global", "mission", Value(std::string("skalitz")));
  server.broadcast_dirty();
  pump(clients, 4);
  for (auto& cl : clients) {
    KCD2S_CHECK_EQ(cl->store.find("global")->get("mission")->as_str(), std::string("skalitz"));
    bool saw = false;
    for (auto& ch : cl->changes)
      if (ch.key == "mission") {
        saw = true;
        KCD2S_CHECK_EQ(ch.old_v.as_str(), std::string("rattay"));
        KCD2S_CHECK_EQ(ch.new_v.as_str(), std::string("skalitz"));
      }
    KCD2S_CHECK(saw);
  }
}

KCD2S_TEST(shared_state_sync, late_joiner_gets_full_snapshot) {
  SharedStateStore server_store;
  ServerSharedStates server(server_store);
  std::vector<std::unique_ptr<Client>> clients;
  clients.push_back(join(server));

  server.set("global", "weather", Value(std::string("rain")));
  server.set("entity:42", "hp", Value(std::int64_t{80}));
  server.broadcast_dirty();
  pump(clients, 3);

  // A client joining now must receive the current state as a full snapshot.
  clients.push_back(join(server));
  pump(clients, 3);

  const auto& late = clients.back();
  KCD2S_CHECK_EQ(late->store.find("global")->get("weather")->as_str(), std::string("rain"));
  KCD2S_CHECK_EQ(late->store.find("entity:42")->get("hp")->as_int(), std::int64_t{80});
}

KCD2S_TEST(shared_state_sync, forged_client_write_to_server_bag_rejected) {
  SharedStateStore server_store;
  ServerSharedStates server(server_store);
  std::vector<std::unique_ptr<Client>> clients;
  clients.push_back(join(server));

  server.set("global", "locked", Value(std::string("yes")));  // Server-authority bag
  server.broadcast_dirty();
  pump(clients, 3);

  // Client tries to overwrite a server bag -> server author_write(is_server=false) rejects it.
  clients[0]->repl->propose("global", "locked", Value(std::string("hacked")));
  pump(clients, 4);

  KCD2S_CHECK_EQ(server_store.find("global")->get("locked")->as_str(), std::string("yes"));
  KCD2S_CHECK_EQ(clients[0]->store.find("global")->get("locked")->as_str(), std::string("yes"));
}

KCD2S_TEST(shared_state_sync, client_authority_bag_write_flows_and_fans_out) {
  SharedStateStore server_store;
  ServerSharedStates server(server_store);
  std::vector<std::unique_ptr<Client>> clients;
  clients.push_back(join(server));
  clients.push_back(join(server));

  // Server declares a Client-authority bag (empty) and snapshots it.
  server.set("cfg", "_", Value(std::int64_t{0}), BagAuthority::Client);
  server.broadcast_dirty();
  pump(clients, 3);

  // Client 0 proposes a write -> accepted -> fanned out to both clients.
  clients[0]->repl->propose("cfg", "volume", Value(std::int64_t{7}));
  pump(clients, 5);

  KCD2S_CHECK_EQ(server_store.find("cfg")->get("volume")->as_int(), std::int64_t{7});
  for (auto& cl : clients)
    KCD2S_CHECK_EQ(cl->store.find("cfg")->get("volume")->as_int(), std::int64_t{7});
}
