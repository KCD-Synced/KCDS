#include "kcd2s/integration/session.hpp"

#include <string>

#include "kcd2s/replication/world.hpp"

namespace kcd2s::integration {

using replication::Authority;
using replication::FieldId;
using replication::FieldKind;
using replication::FieldValue;
using replication::Priority;
using replication::SchemaRegistry;
using replication::Vec3;
using sdk::Tag;
using sdk::Value;
using transport::Connection;
using transport::make_sim_pair;

namespace {

SchemaRegistry make_player_schema() {
  SchemaRegistry reg;
  reg.define("Player")
      .field("position", FieldKind::Vec3, Priority::High, Authority::Owner)  // field 0 (owner)
      .field("health", FieldKind::F32, Priority::Med, Authority::Server);    // field 1 (server)
  return reg;
}

std::string roster_key(PeerId pid) {
  return "player:" + std::to_string(pid);
}

const std::uint8_t kEv = static_cast<std::uint8_t>(wire::Channel::Events);

}  // namespace

Session::Session()
    : schema_(make_player_schema()),
      player_t_(schema_.get("Player")->id),
      server_(schema_, replication::InterestConfig{128.0f}),
      server_bags_(server_store_) {
  server_.world().set_tick(1);
}

Session::Member& Session::member(PeerId pid) const {
  return *members_.at(pid);
}

PeerId Session::join(const std::string& name) {
  replication::World& w = server_.world();

  // Server spawns + owns the avatar (authoritative). Spread spawns so positions differ.
  const core::EntityId av = w.spawn(player_t_);
  const float x = static_cast<float>(++spawn_counter_) * 10.0f;
  w.write(av, 0, Vec3{x, 0.0f, 0.0f});
  w.write(av, 1, 100.0f);

  auto m = std::make_unique<Member>();
  m->name = name;
  m->avatar = av;

  // Replication link pair: server uses rs, client uses rc.
  auto rp = make_sim_pair({}, {});
  m->rs = std::move(rp.first);
  m->rc = std::move(rp.second);
  const PeerId pid = server_.add_client(m->rs.get(), av, 1.0e6f);  // see-all range
  m->pid = pid;
  server_.set_owner(av, pid);
  m->peer = std::make_unique<ClientPeer>(schema_, m->rc.get());

  // Bus link pair (events: chat + state-bags): server uses bs, client uses bc.
  auto bp = make_sim_pair({}, {});
  m->bs = std::move(bp.first);
  m->bc = std::move(bp.second);
  m->sbus_conn = std::make_unique<Connection>(m->bs.get());
  m->cbus_conn = std::make_unique<Connection>(m->bc.get());
  m->sbus = std::make_unique<NetBus>(*m->sbus_conn);
  m->cbus = std::make_unique<NetBus>(*m->cbus_conn);

  Member* raw = m.get();
  // Client applies roster deltas and logs changes (key + +/- for set/remove).
  m->client_bags = std::make_unique<ClientSharedStates>(
      m->client_store, *m->cbus,
      [raw](const std::string& bag, const std::string& key, const Value&, const Value& n) {
        (void)bag;
        raw->roster_changes.push_back(key + (n.is_null() ? ":-" : ":+"));
      });
  // Client receives chat.
  m->cbus->on("chat", [raw](const Value::Array& a) {
    if (a.size() == 2 && a[0].tag() == Tag::Str && a[1].tag() == Tag::Str)
      raw->chat_log.push_back(a[0].as_str() + ": " + a[1].as_str());
  });
  // Server receives a client's chat and rebroadcasts to everyone.
  m->sbus->on("chat", [this](const Value::Array& a) { broadcast_chat(a); });

  server_bags_.add_client(*m->sbus);  // sends the current roster snapshot to the new client

  members_.emplace(pid, std::move(m));

  // Add the player to the authoritative roster (replicates on the next broadcast_dirty()).
  server_bags_.set("players", roster_key(pid), Value(name));
  return pid;
}

void Session::leave(PeerId pid) {
  auto it = members_.find(pid);
  if (it == members_.end())
    return;
  Member& m = *it->second;

  server_.despawn(m.avatar);                       // clean DESTRUCT to remaining clients
  server_bags_.erase("players", roster_key(pid));  // roster removal replicates
  server_bags_.remove_client(*m.sbus);             // stop replicating to the leaver before freeing
  server_.remove_client(pid);                      // stop snapshots to it (no entities left to own)
  members_.erase(it);
}

void Session::move(PeerId pid, const Vec3& pos) {
  member(pid).peer->send_input(member(pid).avatar, {{FieldId{0}, FieldValue{pos}}});
}

void Session::say(PeerId pid, const std::string& msg) {
  Member& m = member(pid);
  m.cbus->emit("chat", Value::Array{Value(m.name), Value(msg)});
}

void Session::broadcast_chat(const Value::Array& args) {
  for (auto& [pid, m] : members_) {
    (void)pid;
    m->sbus->emit("chat", args);
  }
}

void Session::tick(int n) {
  for (int i = 0; i < n; ++i) {
    now_ += 1.0 / 60.0;
    replication::World& w = server_.world();
    w.set_tick(w.tick() + 1);

    server_bags_.broadcast_dirty();  // queue any roster deltas onto the server bus connections
    server_.tick(now_);              // replication snapshots + control

    for (auto& [pid, m] : members_) {
      (void)pid;
      m->peer->tick(now_);
      m->sbus_conn->flush(now_);
      m->cbus_conn->flush(now_);
      for (auto& msg : m->sbus_conn->receive(now_))
        if (msg.channel == kEv)
          m->sbus->feed(msg.bytes);
      for (auto& msg : m->cbus_conn->receive(now_))
        if (msg.channel == kEv)
          m->cbus->feed(msg.bytes);
    }
  }
}

const replication::Mirror& Session::mirror(PeerId pid) const {
  return member(pid).peer->mirror();
}

core::EntityId Session::avatar_of(PeerId pid) const {
  return member(pid).avatar;
}

std::size_t Session::players_visible_to(PeerId pid) const {
  std::size_t n = 0;
  for (const auto& [e, ent] : mirror(pid).entities())
    if (ent.type == player_t_) {
      (void)e;
      ++n;
    }
  return n;
}

std::size_t Session::roster_size(PeerId pid) const {
  const sdk::SharedState* b = member(pid).client_store.find("players");
  return b ? b->size() : 0;
}

bool Session::roster_has(PeerId pid, PeerId who) const {
  const sdk::SharedState* b = member(pid).client_store.find("players");
  return b && b->get(roster_key(who)) != nullptr;
}

const std::vector<std::string>& Session::chat_log(PeerId pid) const {
  return member(pid).chat_log;
}

const std::vector<std::string>& Session::roster_changes(PeerId pid) const {
  return member(pid).roster_changes;
}

}  // namespace kcd2s::integration
