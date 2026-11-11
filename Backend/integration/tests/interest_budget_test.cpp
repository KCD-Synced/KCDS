// Bounded interest, snapshot byte budgets and fragmentation (phase 2).
//
// The game host measures interest in the horizontal (x, y) plane of the Z-up KCD2 world with a
// bounded radius and hysteresis, filters by dimension and visibility before serialization,
// keeps a client's avatar and its owned in-scope entities in view, spends a per-peer and aggregate
// snapshot byte budget by priority, and splits datagrams above the safe UDP payload into verbatim
// fragments.
#include "../local/movement.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/npc.hpp"
#include "kcd2s/integration/protocol.hpp"
#include "kcd2s/integration/server.hpp"
#include "kcd2s/replication/interest.hpp"
#include "kcd2s/replication/snapshot.hpp"
#include "kcd2s/testkit.hpp"
#include "kcd2s/transport/connection.hpp"
#include "kcd2s/transport/datagram.hpp"
#include "kcd2s/transport/fragment.hpp"

using namespace kcd2s;
using namespace kcd2s::integration;
using core::EntityId;
using core::Vec3;
using replication::InterestConfig;
using replication::UpAxis;
using transport::FragmentFraming;
using transport::FragmentingLink;
using transport::IDatagramLink;
using transport::LinkParams;
using transport::SimLink;

namespace {

constexpr float kRange = 150.0f;
constexpr float kHysteresis = 25.0f;
constexpr std::size_t kMtu = 1200;

// Forwards to another link and remembers the largest datagram handed to the wire, how many
// there were and a digest of their bytes in order.
class RecordingLink : public IDatagramLink {
 public:
  explicit RecordingLink(IDatagramLink* inner) : inner_(inner) {
  }
  using IDatagramLink::send;
  void send(const std::uint8_t* data, std::size_t len) override {
    largest = std::max(largest, len);
    datagrams += 1;
    for (std::size_t i = 0; i < len; ++i) {
      digest = (digest ^ data[i]) * 0x100000001b3ull;  // FNV-1a
    }
    digest = (digest ^ 0xff) * 0x100000001b3ull;  // datagram boundary
    inner_->send(data, len);
  }
  bool recv(transport::Datagram& out) override {
    return inner_->recv(out);
  }
  void poll() override {
    inner_->poll();
  }
  std::size_t largest = 0;
  std::uint64_t datagrams = 0;
  std::uint64_t digest = 0xcbf29ce484222325ull;

 private:
  IDatagramLink* inner_;
};

struct Peer {
  std::unique_ptr<SimLink> server_side;
  std::unique_ptr<SimLink> client_side;
  std::unique_ptr<RecordingLink> server_wire;
  std::unique_ptr<FragmentingLink> server_frames;
  std::unique_ptr<FragmentingLink> client_frames;
  std::unique_ptr<ClientPeer> client;
  PeerId id = 0;
  EntityId avatar = 0;
};

class Harness {
 public:
  Harness() : schema_(local::player_schema()), server_(schema_, InterestConfig{64.0f, UpAxis::Z}) {
    server_.set_interest_hysteresis(kHysteresis);
    server_.world().set_tick(1);
  }

  Server& server() {
    return server_;
  }

  // A connected client whose avatar starts at `at`. With `fragmenting`, both ends use the verbatim
  // fragmentation of the UDP host and client.
  Peer& add_peer(Vec3 at, bool fragmenting = false, LinkParams link = {}) {
    auto peer = std::make_unique<Peer>();
    auto links = transport::make_sim_pair(link, link);
    peer->server_side = std::move(links.first);
    peer->client_side = std::move(links.second);
    peer->server_wire = std::make_unique<RecordingLink>(peer->server_side.get());
    IDatagramLink* server_link = peer->server_wire.get();
    IDatagramLink* client_link = peer->client_side.get();
    if (fragmenting) {
      peer->server_frames =
          std::make_unique<FragmentingLink>(server_link, kMtu, FragmentFraming::Verbatim);
      peer->client_frames =
          std::make_unique<FragmentingLink>(client_link, kMtu, FragmentFraming::Verbatim);
      server_link = peer->server_frames.get();
      client_link = peer->client_frames.get();
    }
    peer->avatar = local::spawn_player(server_.world());
    server_.world().write(peer->avatar, local::Position, at);
    peer->id = server_.add_client(server_link, peer->avatar, kRange);
    server_.set_owner(peer->avatar, peer->id);
    peer->client = std::make_unique<ClientPeer>(schema_, client_link);
    peers_.push_back(std::move(peer));
    return *peers_.back();
  }

  // A server-owned player avatar with no client (an NPC-like remote player).
  EntityId add_remote(Vec3 at) {
    const EntityId e = local::spawn_player(server_.world());
    server_.world().write(e, local::Position, at);
    return e;
  }

  // The world tick is advanced after each server tick, so writes made between pumps are stamped
  // with the tick whose snapshot carries them (as the host stamps upstream input). `before_tick`
  // runs before every server tick (clients sending input).
  void pump(int ticks, const std::function<void(int)>& before_tick = {}) {
    for (int i = 0; i < ticks; ++i) {
      if (before_tick) {
        before_tick(i);
      }
      now_ += 1.0 / 60.0;
      for (auto& p : peers_) {
        p->server_side->poll();
        p->client_side->poll();
      }
      server_.tick(now_);
      for (auto& p : peers_)
        p->client->tick(now_);
      server_.world().set_tick(server_.world().tick() + 1);
    }
  }

  const replication::Mirror::Entity* seen(const Peer& p, EntityId e) const {
    return p.client->mirror().get(e);
  }

  void move(EntityId e, Vec3 to) {
    server_.world().write(e, local::Position, to);
  }

 private:
  replication::SchemaRegistry schema_;
  Server server_;
  std::vector<std::unique_ptr<Peer>> peers_;
  double now_ = 0.0;
};

Vec3 position_of(const replication::Mirror::Entity& entity) {
  return std::get<Vec3>(entity.fields[local::Position]);
}

}  // namespace

KCD2S_TEST(interest_budget, enter_and_leave_with_hysteresis_in_the_xy_plane) {
  Harness h;
  Peer& viewer = h.add_peer(Vec3{0, 0, 0});
  const EntityId other = h.add_remote(Vec3{140, 0, 0});
  h.pump(5);
  KCD2S_CHECK(h.seen(viewer, other) != nullptr);  // inside the 150 m range

  h.move(other, Vec3{160, 0, 0});
  h.pump(5);
  KCD2S_CHECK(h.seen(viewer, other) != nullptr);  // beyond the range, inside the hysteresis

  h.move(other, Vec3{180, 0, 0});
  h.pump(5);
  KCD2S_CHECK(h.seen(viewer, other) == nullptr);  // beyond range + hysteresis: left

  h.move(other, Vec3{160, 0, 0});
  h.pump(5);
  KCD2S_CHECK(h.seen(viewer, other) == nullptr);  // must come within the range to enter again

  h.move(other, Vec3{0, 140, 900});  // y is horizontal, z (height) is ignored
  h.pump(5);
  KCD2S_CHECK(h.seen(viewer, other) != nullptr);

  h.move(other, Vec3{0, 400, 0});
  h.pump(5);
  KCD2S_CHECK(h.seen(viewer, other) == nullptr);
}

KCD2S_TEST(interest_budget, reentry_constructs_full_current_state) {
  Harness h;
  Peer& viewer = h.add_peer(Vec3{0, 0, 0});
  const EntityId other = h.add_remote(Vec3{20, 0, 0});
  h.pump(5);
  KCD2S_CHECK(h.seen(viewer, other) != nullptr);

  h.move(other, Vec3{500, 0, 0});
  h.pump(5);
  KCD2S_CHECK(h.seen(viewer, other) == nullptr);
  // State changes while out of view are never sent as UPDATEs...
  h.server().world().write(other, local::Health, 42.0f);
  h.server().world().write(other, local::Animations, std::string(1220, 'a'));
  h.pump(3);

  // ...so re-entry must CONSTRUCT the entity with its full current state.
  h.move(other, Vec3{30, 0, 0});
  h.pump(5);
  const auto* entity = h.seen(viewer, other);
  KCD2S_CHECK(entity != nullptr);
  if (entity) {
    KCD2S_CHECK(std::get<float>(entity->fields[local::Health]) == 42.0f);
    KCD2S_CHECK(std::get<std::string>(entity->fields[local::Animations]) == std::string(1220, 'a'));
    KCD2S_CHECK(position_of(*entity).x == 30.0f);
  }
}

KCD2S_TEST(interest_budget, reentry_waits_for_the_destruct_acknowledgement) {
  // Drive the client side by hand so acknowledgements can be withheld. Without the wait, an entity
  // that re-enters before its DESTRUCT is acknowledged would get an UPDATE for an entity the client
  // already removed, and the whole snapshot would be rejected.
  auto schema = local::player_schema();
  Server server(schema, InterestConfig{64.0f, UpAxis::Z});
  server.world().set_tick(1);
  auto links = transport::make_loopback_pair();
  const EntityId viewer = local::spawn_player(server.world());
  const EntityId other = local::spawn_player(server.world());
  server.world().write(other, local::Position, Vec3{10, 0, 0});
  server.add_client(links.first.get(), viewer, kRange);
  transport::Connection client(links.second.get());
  client.set_channel_tier(kChannelReplAck, transport::DeliveryTier::UnreliableSequenced);
  replication::Mirror mirror(schema);

  double now = 0.0;
  bool every_snapshot_applied = true;
  auto step = [&](bool acknowledge) {
    now += 1.0 / 60.0;
    server.world().set_tick(server.world().tick() + 1);
    server.tick(now);
    for (const auto& message : client.receive(now)) {
      if (message.channel == static_cast<std::uint8_t>(wire::Channel::Snapshot)) {
        if (!mirror.apply(message.bytes))
          every_snapshot_applied = false;
      }
    }
    if (acknowledge)
      client.queue(kChannelReplAck, encode_tick(mirror.last_applied()));
    client.flush(now);
  };

  for (int i = 0; i < 4; ++i)
    step(true);
  KCD2S_CHECK(mirror.get(other) != nullptr);

  // Leave, then come straight back while every acknowledgement is withheld.
  server.world().write(other, local::Position, Vec3{900, 0, 0});
  step(false);
  KCD2S_CHECK(mirror.get(other) == nullptr);
  server.world().write(other, local::Position, Vec3{12, 0, 0});
  server.world().write(other, local::Health, 7.0f);
  for (int i = 0; i < 4; ++i)
    step(false);
  KCD2S_CHECK(every_snapshot_applied);
  KCD2S_CHECK(mirror.get(other) == nullptr);  // still waiting for the DESTRUCT acknowledgement

  for (int i = 0; i < 4; ++i)
    step(true);
  KCD2S_CHECK(every_snapshot_applied);
  const auto* entity = mirror.get(other);
  KCD2S_CHECK(entity != nullptr);
  if (entity) {
    KCD2S_CHECK(std::get<float>(entity->fields[local::Health]) == 7.0f);
    KCD2S_CHECK(position_of(*entity).x == 12.0f);
  }
}

KCD2S_TEST(interest_budget, dimension_and_visibility_filter_before_serialization) {
  Harness h;
  Peer& viewer = h.add_peer(Vec3{0, 0, 0});
  const EntityId other_world = h.add_remote(Vec3{5, 0, 0});
  const EntityId hidden = h.add_remote(Vec3{6, 0, 0});
  const EntityId landmark = h.add_remote(Vec3{5000, 0, 0});
  const EntityId owned_far = h.add_remote(Vec3{0, 6000, 0});
  const EntityId owned_elsewhere = h.add_remote(Vec3{7, 0, 0});
  const EntityId private_to_other = h.add_remote(Vec3{8, 0, 0});
  auto& world = h.server().world();
  // Numbered dimensions: the viewer lives in world 1. The global world bridges every world.
  world.slot(viewer.avatar)->dimension = 1;
  world.slot(other_world)->dimension = 3;
  world.slot(hidden)->flags &= static_cast<std::uint8_t>(~replication::kFlagVisible);
  world.slot(landmark)->flags |= replication::kFlagAlwaysVisible;
  world.slot(landmark)->dimension = 1;
  world.slot(owned_far)->owner = viewer.id;
  world.slot(owned_far)->dimension = 1;  // owned entities ignore the range, not the scope
  world.slot(owned_elsewhere)->owner = viewer.id;
  world.slot(owned_elsewhere)->dimension = 3;
  world.slot(private_to_other)->visibleTo = viewer.id + 100;
  h.pump(5);
  KCD2S_CHECK(h.seen(viewer, viewer.avatar) != nullptr);
  KCD2S_CHECK(h.seen(viewer, other_world) == nullptr);
  KCD2S_CHECK(h.seen(viewer, hidden) == nullptr);
  KCD2S_CHECK(h.seen(viewer, landmark) != nullptr);
  KCD2S_CHECK(h.seen(viewer, owned_far) != nullptr);
  // Ownership never carries an entity across a dimension or private visibility boundary.
  KCD2S_CHECK(h.seen(viewer, owned_elsewhere) == nullptr);
  KCD2S_CHECK(h.seen(viewer, private_to_other) == nullptr);

  // Moving the viewer into that dimension swaps what it may see.
  world.slot(viewer.avatar)->dimension = 3;
  h.pump(5);
  KCD2S_CHECK(h.seen(viewer, other_world) != nullptr);
  KCD2S_CHECK(h.seen(viewer, owned_elsewhere) != nullptr);
  KCD2S_CHECK(h.seen(viewer, landmark) == nullptr);
  KCD2S_CHECK(h.seen(viewer, owned_far) == nullptr);
  KCD2S_CHECK(h.seen(viewer, private_to_other) == nullptr);
  KCD2S_CHECK(h.seen(viewer, viewer.avatar) != nullptr);
}

KCD2S_TEST(interest_budget, budget_sends_self_first_and_drains_every_construct) {
  Harness h;
  SnapshotBudget budget;
  budget.peer_bytes_per_tick = 2000;
  h.server().set_snapshot_budget(budget);
  std::vector<EntityId> crowd;
  for (int i = 0; i < 40; ++i) {
    const EntityId e = h.add_remote(Vec3{static_cast<float>(i), 1.0f, 0.0f});
    h.server().world().write(e, local::Animations, std::string(1220, 'x'));
    crowd.push_back(e);
  }
  Peer& viewer = h.add_peer(Vec3{0, 0, 0});
  h.pump(1);
  KCD2S_CHECK(h.seen(viewer, viewer.avatar) != nullptr);  // own avatar in the very first snapshot
  KCD2S_CHECK(viewer.client->mirror().size() < crowd.size());

  h.pump(120);
  std::size_t seen = 0;
  for (EntityId e : crowd) {
    if (h.seen(viewer, e))
      seen += 1;
  }
  KCD2S_CHECK_EQ(seen, crowd.size());
  const SnapshotBudgetCounters counters = h.server().budget_counters();
  KCD2S_CHECK(counters.budget_limited_snapshots > 0);
  KCD2S_CHECK(counters.deferred_constructs > 0);
  // Credit is capped at 4 x 2000 bytes; these constructs fit without authorized overdraw.
  KCD2S_CHECK(counters.largest_snapshot_bytes <= 8000);
  KCD2S_CHECK_EQ(counters.overdraw_snapshots, std::uint64_t{0});
}

KCD2S_TEST(interest_budget, priority_prefers_combat_and_near_without_starving_far) {
  Harness h;
  Peer& viewer = h.add_peer(Vec3{0, 0, 0});
  const EntityId near_one = h.add_remote(Vec3{5, 0, 0});
  const EntityId fighter = h.add_remote(Vec3{120, 0, 0});
  std::vector<EntityId> far;
  for (int i = 0; i < 6; ++i)
    far.push_back(h.add_remote(Vec3{100.0f + static_cast<float>(i), 50.0f, 0.0f}));
  h.pump(10);  // unbudgeted: everything constructed

  SnapshotBudget budget;
  budget.peer_bytes_per_tick = 100;  // room for about two small UPDATEs per tick
  budget.burst_bytes = 200;
  h.server().set_snapshot_budget(budget);
  h.server().note_combat(viewer.avatar, fighter);

  int near_fresh = 0;
  int fighter_fresh = 0;
  int far_fresh = 0;
  int far_worst_gap = 0;
  int far_gap = 0;
  for (int t = 0; t < 60; ++t) {
    // A new value every tick, so a mirror value is current only if this tick updated it.
    const float wobble = 0.25f * static_cast<float>(t + 1);
    h.move(near_one, Vec3{5, wobble, 0});
    h.move(fighter, Vec3{120, wobble, 0});
    for (std::size_t i = 0; i < far.size(); ++i)
      h.move(far[i], Vec3{100.0f + static_cast<float>(i), 50.0f + wobble, 0.0f});
    h.pump(1);
    auto fresh = [&](EntityId e) {
      const auto* entity = h.seen(viewer, e);
      return entity && position_of(*entity).y == h.server().world().slot(e)->position.y;
    };
    near_fresh += fresh(near_one) ? 1 : 0;
    fighter_fresh += fresh(fighter) ? 1 : 0;
    const bool far_updated = fresh(far[0]);
    far_fresh += far_updated ? 1 : 0;
    far_gap = far_updated ? 0 : far_gap + 1;
    far_worst_gap = std::max(far_worst_gap, far_gap);
  }
  std::fprintf(stderr, "    fresh: fighter=%d near=%d far=%d gap=%d\n", fighter_fresh, near_fresh,
               far_fresh, far_worst_gap);
  KCD2S_CHECK(fighter_fresh >= near_fresh);  // combat partner first
  KCD2S_CHECK(near_fresh > far_fresh);       // then the nearer entity
  KCD2S_CHECK(far_fresh > 0);                // starvation lifts the far ones eventually
  KCD2S_CHECK(far_worst_gap < 60);
  KCD2S_CHECK(h.server().budget_counters().deferred_entries > 0);
}

KCD2S_TEST(interest_budget, mandatory_traffic_above_the_budget_never_starves_other_entities) {
  // The viewer's owned entities change by more bytes every tick than its whole budget, so its
  // credit never recovers. Optional entries must still be sent once they have waited long enough.
  Harness h;
  SnapshotBudget budget;
  budget.peer_bytes_per_tick = 300;
  budget.burst_bytes = 600;
  h.server().set_snapshot_budget(budget);
  Peer& viewer = h.add_peer(Vec3{0, 0, 0});
  std::vector<EntityId> owned;
  for (int i = 0; i < 3; ++i) {
    const EntityId e = h.add_remote(Vec3{static_cast<float>(i), 3.0f, 0.0f});
    h.server().set_owner(e, viewer.id);
    owned.push_back(e);
  }
  const EntityId neighbour = h.add_remote(Vec3{10, 0, 0});

  for (int t = 0; t < 180; ++t) {
    const char fill = static_cast<char>('a' + t % 26);
    for (EntityId e : owned)
      h.server().world().write(e, local::Animations, std::string(400, fill));
    h.pump(1);
  }
  for (EntityId e : owned)
    KCD2S_CHECK(h.seen(viewer, e) != nullptr);
  KCD2S_CHECK(h.seen(viewer, neighbour) != nullptr);
  KCD2S_CHECK(h.server().budget_counters().overdraw_snapshots > 0);
}

KCD2S_TEST(interest_budget, large_construct_recovers_while_small_updates_keep_fitting) {
  Harness h;
  LinkParams lossy;
  lossy.loss = 0.1;
  lossy.min_delay = 1;
  lossy.max_delay = 3;
  lossy.seed = 487;
  Peer& viewer = h.add_peer(Vec3{0, 0, 0}, true, lossy);
  const EntityId small = h.add_remote(Vec3{5, 0, 0});
  h.pump(60);
  KCD2S_CHECK(h.seen(viewer, small) != nullptr);
  SnapshotBudget budget;
  budget.peer_bytes_per_tick = 100;
  budget.burst_bytes = 200;
  h.server().set_snapshot_budget(budget);
  const EntityId large = h.add_remote(Vec3{10, 0, 0});
  h.server().world().write(large, local::Animations, std::string(1220, 'n'));
  int first_seen = -1;
  for (int tick = 0; tick < 240; ++tick) {
    // A small lower-priority update can always use some credit. It must not
    // make an entry larger than the entire burst wait forever.
    h.move(small, Vec3{5, float(tick % 10) / 4, 0});
    h.pump(1);
    if (first_seen < 0 && h.seen(viewer, large)) {
      first_seen = tick;
    }
  }
  KCD2S_CHECK(first_seen >= 0);
  KCD2S_CHECK(first_seen < 180);
  KCD2S_CHECK(h.server().budget_counters().overdraw_snapshots > 0);
  // Once the CONSTRUCT is confirmed, an unchanged large entity stops consuming
  // construction attempts despite continuing traffic and packet loss.
  const auto before = h.server().budget_counters().deferred_constructs;
  h.pump(60);
  KCD2S_CHECK_EQ(h.server().budget_counters().deferred_constructs, before);
  KCD2S_CHECK(h.seen(viewer, large) != nullptr);
}

KCD2S_TEST(interest_budget, deferral_age_survives_overload_then_resets_after_departure) {
  Harness h;
  Peer& viewer = h.add_peer(Vec3{0, 0, 0});
  SnapshotBudget budget;
  budget.peer_bytes_per_tick = 100;
  budget.burst_bytes = 200;
  h.server().set_snapshot_budget(budget);
  const EntityId other = h.add_remote(Vec3{10, 0, 0});
  h.server().world().write(other, local::Animations, std::string(400, 'n'));
  auto overloaded_tick = [&]() {
    h.server().world().write(viewer.avatar, local::Animations,
                             std::string(400, char('a' + h.server().world().tick() % 26)));
    h.pump(1);
  };
  int first_seen = -1;
  for (int tick = 0; tick < 70; ++tick) {
    overloaded_tick();
    if (first_seen < 0 && h.seen(viewer, other)) {
      first_seen = tick;
    }
  }
  KCD2S_CHECK(first_seen >= 60 && first_seen < 65);
  const EntityId waiting = h.add_remote(Vec3{20, 0, 0});
  h.server().world().write(waiting, local::Animations, std::string(400, 'w'));
  for (int tick = 0; tick < 40; ++tick) {
    overloaded_tick();
  }
  KCD2S_CHECK(h.seen(viewer, waiting) == nullptr);
  h.move(waiting, Vec3{1000, 0, 0});
  for (int tick = 0; tick < 3; ++tick) {
    overloaded_tick();
  }
  KCD2S_CHECK(h.seen(viewer, waiting) == nullptr);
  h.move(waiting, Vec3{20, 0, 0});
  for (int tick = 0; tick < 55; ++tick) {
    overloaded_tick();
  }
  KCD2S_CHECK(h.seen(viewer, waiting) == nullptr);  // No stale pre-departure wait age.
  for (int tick = 0; tick < 10; ++tick) {
    overloaded_tick();
  }
  KCD2S_CHECK(h.seen(viewer, waiting) != nullptr);
}

KCD2S_TEST(interest_budget, aggregate_budget_splits_evenly_between_peers) {
  Harness h;
  SnapshotBudget budget;
  budget.peer_bytes_per_tick = 4000;
  budget.total_bytes_per_tick = 3000;  // 1500 per peer with two peers
  h.server().set_snapshot_budget(budget);
  for (int i = 0; i < 20; ++i) {
    const EntityId e = h.add_remote(Vec3{static_cast<float>(i), 2.0f, 0.0f});
    h.server().world().write(e, local::Animations, std::string(1220, 'y'));
  }
  Peer& a = h.add_peer(Vec3{0, 0, 0});
  Peer& b = h.add_peer(Vec3{1, 0, 0});
  h.pump(200);
  const SnapshotBudgetCounters counters = h.server().budget_counters();
  KCD2S_CHECK(counters.aggregate_limited_ticks > 0);
  // Long-run spend stays within the aggregate rate plus each peer's initial burst credit.
  KCD2S_CHECK(counters.snapshot_bytes <= counters.ticks * 3000 + 2 * 6000);
  KCD2S_CHECK_EQ(a.client->mirror().size(), std::size_t{22});
  KCD2S_CHECK_EQ(b.client->mirror().size(), std::size_t{22});
}

KCD2S_TEST(interest_budget, fragmented_datagrams_stay_within_the_safe_payload) {
  Harness h;
  SnapshotBudget budget;
  budget.peer_bytes_per_tick = 8192;
  h.server().set_snapshot_budget(budget);
  std::vector<Peer*> peers;
  for (int i = 0; i < 6; ++i) {
    Peer& p = h.add_peer(Vec3{static_cast<float>(i), 0, 0}, true);
    h.server().world().write(p.avatar, local::Animations,
                             std::string(1220, static_cast<char>('a' + i)));
    peers.push_back(&p);
  }
  h.pump(60);
  for (Peer* p : peers) {
    KCD2S_CHECK(p->server_wire->largest <= kMtu);
    KCD2S_CHECK(p->server_frames->largest_frame() <= kMtu);
    KCD2S_CHECK_EQ(p->client->mirror().size(), peers.size());
    for (std::size_t i = 0; i < peers.size(); ++i) {
      const auto* entity = h.seen(*p, peers[i]->avatar);
      KCD2S_CHECK(entity != nullptr);
      if (entity) {
        KCD2S_CHECK(std::get<std::string>(entity->fields[local::Animations]) ==
                    std::string(1220, static_cast<char>('a' + i)));
      }
    }
  }
  KCD2S_CHECK(peers.front()->server_frames->fragmented_datagrams() > 0);
}

KCD2S_TEST(interest_budget, reliable_chat_survives_loss_fragmentation_and_budget) {
  Harness h;
  SnapshotBudget budget;
  budget.peer_bytes_per_tick = 1500;
  h.server().set_snapshot_budget(budget);
  for (int i = 0; i < 30; ++i) {
    const EntityId e = h.add_remote(Vec3{static_cast<float>(i), 3.0f, 0.0f});
    h.server().world().write(e, local::Animations, std::string(1220, 'z'));
  }
  LinkParams lossy;
  lossy.loss = 0.05;
  lossy.min_delay = 1;
  lossy.max_delay = 3;
  lossy.seed = 77;
  Peer& peer = h.add_peer(Vec3{0, 0, 0}, true, lossy);
  h.server().configure_world(peer.id, WorldConfig{});
  h.pump(60);
  KCD2S_CHECK(peer.client->mark_world_ready(1));
  h.pump(60);
  KCD2S_CHECK(h.server().player_ready(peer.avatar));

  std::vector<std::string> received;
  for (int i = 0; i < 60; ++i) {
    h.server().send_chat(peer.avatar, ChatMessage{"Server", "message " + std::to_string(i), 0});
    h.pump(2);
    for (const auto& message : peer.client->take_chat())
      received.push_back(message.text);
  }
  for (int i = 0; i < 300 && received.size() < 60; ++i) {
    h.pump(1);
    for (const auto& message : peer.client->take_chat())
      received.push_back(message.text);
  }
  KCD2S_CHECK_EQ(received.size(), std::size_t{60});
  bool in_order = received.size() == 60;
  for (std::size_t i = 0; i < received.size(); ++i) {
    if (received[i] != "message " + std::to_string(i))
      in_order = false;
  }
  KCD2S_CHECK(in_order);
  KCD2S_CHECK(h.server().budget_counters().deferred_entries > 0);
  KCD2S_CHECK(peer.server_wire->largest <= kMtu);
}

KCD2S_TEST(interest_budget, parallel_build_matches_serial_with_budgets_and_hysteresis) {
  auto schema = local::player_schema();
  auto populate = [&](Server& server, std::vector<std::unique_ptr<SimLink>>& links) {
    SnapshotBudget budget;
    budget.peer_bytes_per_tick = 3000;
    budget.total_bytes_per_tick = 20000;
    server.set_snapshot_budget(budget);
    server.set_interest_hysteresis(kHysteresis);
    auto& world = server.world();
    world.set_tick(1);
    for (int i = 0; i < 120; ++i) {
      const EntityId e = local::spawn_player(world);
      world.write(
          e, local::Position,
          Vec3{static_cast<float>(i % 12) * 30.0f, static_cast<float>(i / 12) * 30.0f, 0.0f});
      world.write(e, local::Animations, std::string(300, static_cast<char>('a' + i % 20)));
    }
    for (int c = 0; c < 11; ++c) {
      const EntityId viewer = local::spawn_player(world);
      world.write(viewer, local::Position, Vec3{static_cast<float>(c) * 40.0f, 60.0f, 0.0f});
      auto pair = transport::make_sim_pair({}, {});
      const PeerId peer = server.add_client(pair.first.get(), viewer, kRange);
      server.set_owner(viewer, peer);
      links.push_back(std::move(pair.first));
      links.push_back(std::move(pair.second));
    }
  };
  Server serial(schema, InterestConfig{75.0f, UpAxis::Z});
  Server parallel(schema, InterestConfig{75.0f, UpAxis::Z});
  std::vector<std::unique_ptr<SimLink>> serial_links;
  std::vector<std::unique_ptr<SimLink>> parallel_links;
  populate(serial, serial_links);
  populate(parallel, parallel_links);
  parallel.enable_parallel(4);
  KCD2S_CHECK_EQ(parallel.snapshot_workers(), std::size_t{4});

  bool identical = true;
  for (core::Tick tick = 1; tick <= 20; ++tick) {
    for (Server* server : {&serial, &parallel}) {
      auto& world = server->world();
      world.set_tick(tick);
      // Move two columns of players back and forth across the interest edge.
      for (EntityId e : world.live_entities()) {
        const Vec3 at = world.slot(e)->position;
        if (at.x == 330.0f || at.x == 130.0f) {
          const float x = tick % 2 ? 130.0f : 330.0f;
          world.write(e, local::Position, Vec3{x, at.y, 0.0f});
        }
      }
      if (tick == 5) {
        const auto live = world.live_entities();
        server->note_combat(live[3], live[7]);
      }
    }
    const auto a = serial.build_all(tick);
    const auto b = parallel.build_all(tick);
    if (a != b)
      identical = false;
  }
  KCD2S_CHECK(identical);
  const SnapshotBudgetCounters counters = serial.budget_counters();
  KCD2S_CHECK(counters.deferred_entries > 0);
  KCD2S_CHECK(counters.aggregate_limited_ticks > 0);
}

KCD2S_TEST(interest_budget, chunk_membership_does_not_leak_private_or_recycled_entities) {
  Harness h;
  h.server().enable_parallel(3);
  std::vector<Peer*> peers;
  std::vector<EntityId> owned;
  auto& world = h.server().world();
  for (unsigned i = 0; i < 7; ++i) {
    auto& peer = h.add_peer({0, 0, 0});
    peers.push_back(&peer);
    h.server().set_dimension(peer.avatar, i + 1);
    const auto entity = h.add_remote({10, 0, 0});
    owned.push_back(entity);
    h.server().set_dimension(entity, i + 1);
    h.server().set_visible_to(entity, peer.avatar);
    h.server().set_owner(entity, peer.id);
    world.slot(entity)->flags |= replication::kFlagAlwaysVisible;
  }
  const auto global = h.add_remote({20, 0, 0});
  const auto private_entity = h.add_remote({15, 0, 0});
  h.server().set_visible_to(private_entity, peers[0]->avatar);
  h.pump(8);
  for (unsigned i = 0; i < peers.size(); ++i) {
    std::vector<EntityId> expected{peers[i]->avatar, owned[i], global};
    if (i == 0) {
      expected.push_back(private_entity);
    }
    std::sort(expected.begin(), expected.end());
    KCD2S_CHECK(h.server().visible_entities(peers[i]->id) == expected);
    KCD2S_CHECK_EQ(peers[i]->client->mirror().entities().size(), expected.size());
  }
  const auto recycled_row = world.row_index(private_entity);
  h.server().despawn(private_entity);
  const auto replacement = h.add_remote({15, 0, 0});
  KCD2S_CHECK_EQ(world.row_index(replacement), recycled_row);
  h.server().set_visible_to(replacement, peers.back()->avatar);
  // More workers than viewers changes chunk ownership. No previous viewer's stamp may survive.
  h.server().enable_parallel(12);
  h.pump(8);
  for (unsigned i = 0; i < peers.size(); ++i) {
    KCD2S_CHECK(h.seen(*peers[i], private_entity) == nullptr);
    KCD2S_CHECK_EQ(h.seen(*peers[i], replacement) != nullptr, i + 1 == peers.size());
    std::vector<EntityId> expected{peers[i]->avatar, owned[i], global};
    if (i + 1 == peers.size()) {
      expected.push_back(replacement);
    }
    std::sort(expected.begin(), expected.end());
    KCD2S_CHECK(h.server().visible_entities(peers[i]->id) == expected);
  }
}

KCD2S_TEST(interest_budget, parallel_tick_matches_serial) {
  // Whole ticks: clients send owner input and acknowledgements, the server receives, applies,
  // builds and sends. With snapshot workers it does the per-client phases in parallel; every
  // client must still get the same datagrams and the world the same state.
  Harness serial;
  Harness parallel;
  parallel.server().enable_parallel(4);
  parallel.server().set_snapshot_profiling(true);
  std::vector<Peer*> serial_peers;
  std::vector<Peer*> parallel_peers;
  for (int i = 0; i < 12; ++i) {
    const Vec3 at{static_cast<float>(i % 4) * 40.0f, static_cast<float>(i / 4) * 40.0f, 0.0f};
    serial_peers.push_back(&serial.add_peer(at, true));
    parallel_peers.push_back(&parallel.add_peer(at, true));
  }
  for (Harness* h : {&serial, &parallel}) {
    for (int i = 0; i < 6; ++i) {
      const EntityId e = h->add_remote(Vec3{static_cast<float>(i) * 25.0f, 20.0f, 0.0f});
      h->server().world().write(e, local::Animations, std::string(900, char('a' + i)));
    }
  }
  auto inputs = [](std::vector<Peer*>& peers) {
    return [&peers](int tick) {
      for (std::size_t k = 0; k < peers.size(); ++k) {
        const float step = static_cast<float>(tick) * 0.5f;
        const Vec3 at{static_cast<float>(k % 4) * 40.0f + step,
                      static_cast<float>(k / 4) * 40.0f - step, 0.0f};
        peers[k]->client->send_input(peers[k]->avatar, {{local::Position, at}});
      }
    };
  };
  serial.pump(40, inputs(serial_peers));
  parallel.pump(40, inputs(parallel_peers));
  KCD2S_CHECK_EQ(serial.server().snapshot_profile_counters().snapshots, std::uint64_t{0});
  KCD2S_CHECK_EQ(parallel.server().snapshot_profile_counters().snapshots, std::uint64_t{480});

  for (std::size_t k = 0; k < serial_peers.size(); ++k) {
    const Peer& a = *serial_peers[k];
    const Peer& b = *parallel_peers[k];
    KCD2S_CHECK(a.server_wire->datagrams > 40);
    KCD2S_CHECK_EQ(a.server_wire->datagrams, b.server_wire->datagrams);
    KCD2S_CHECK_EQ(a.server_wire->digest, b.server_wire->digest);
    const auto* moved = serial.server().world().read(a.avatar, local::Position);
    const auto* same = parallel.server().world().read(b.avatar, local::Position);
    KCD2S_CHECK(moved && same && std::get<Vec3>(*moved) == std::get<Vec3>(*same));
    // The input arrived: the avatar left its starting point.
    KCD2S_CHECK(std::get<Vec3>(*moved).x > static_cast<float>(k % 4) * 40.0f);
  }
  parallel.server().set_snapshot_profiling(false);
  parallel.pump(2);
  KCD2S_CHECK_EQ(parallel.server().snapshot_profile_counters().snapshots, std::uint64_t{480});
}

KCD2S_TEST(interest_budget, entering_entity_outranks_updates_of_visible_ones) {
  Harness h;
  Peer& viewer = h.add_peer(Vec3{0, 0, 0});
  std::vector<EntityId> crowd;
  for (int i = 0; i < 12; ++i)
    crowd.push_back(h.add_remote(Vec3{2.0f + static_cast<float>(i), 0.0f, 0.0f}));
  h.pump(10);  // unbudgeted: the crowd is constructed

  SnapshotBudget budget;
  budget.peer_bytes_per_tick = 1600;  // roughly one construct, or the crowd's updates
  budget.burst_bytes = 1600;
  h.server().set_snapshot_budget(budget);
  const EntityId newcomer = h.add_remote(Vec3{60, 0, 0});
  h.server().world().write(newcomer, local::Animations, std::string(1220, 'n'));

  int ticks_until_seen = -1;
  for (int t = 0; t < 30 && ticks_until_seen < 0; ++t) {
    const float step = 0.25f * static_cast<float>(t + 1);
    for (std::size_t i = 0; i < crowd.size(); ++i)
      h.move(crowd[i], Vec3{2.0f + static_cast<float>(i), step, 0.0f});
    h.pump(1);
    if (h.seen(viewer, newcomer))
      ticks_until_seen = t + 1;
  }
  // The crowd's updates alone would fill the budget every tick; the newcomer still gets in fast.
  KCD2S_CHECK(ticks_until_seen > 0);
  KCD2S_CHECK(ticks_until_seen <= 3);
}

// Live: some 230 level people filled nearly every snapshot of a village, and another
// player's turn showed about a second late. Another player goes before every NPC, even nearer ones.
KCD2S_TEST(interest_budget, another_player_goes_before_a_crowd_of_npcs) {
  Harness h;
  Peer& viewer = h.add_peer(Vec3{0, 0, 0});
  const EntityId other = h.add_remote(Vec3{30, 0, 0});
  std::vector<EntityId> crowd;
  for (int i = 0; i < 24; ++i) {
    NpcSpawn spawn;
    spawn.position = core::Vec3{2.0f + static_cast<float>(i) * 0.5f, 3.0f, 0.0f};
    crowd.push_back(h.server().spawn_npc(spawn));
  }
  h.pump(20);  // unbudgeted: everyone is constructed

  SnapshotBudget budget;
  budget.peer_bytes_per_tick = 120;  // room for a few small UPDATEs per tick
  budget.burst_bytes = 240;
  h.server().set_snapshot_budget(budget);
  // The budget's order alone: the distance rates would send the player 30 m away at 20 Hz.
  h.server().set_distance_rates(false);

  int other_fresh = 0;
  for (int t = 0; t < 60; ++t) {
    const float step = 0.25f * static_cast<float>(t + 1);
    for (std::size_t i = 0; i < crowd.size(); ++i) {
      const core::Vec3 at{2.0f + static_cast<float>(i) * 0.5f, 3.0f + step, 0.0f};
      h.server().world().write(crowd[i], NpcPosition, at);
    }
    h.move(other, Vec3{30, step, 0});
    h.pump(1);
    const auto* seen = h.seen(viewer, other);
    if (seen && position_of(*seen).y == h.server().world().slot(other)->position.y) {
      ++other_fresh;
    }
  }
  std::fprintf(stderr, "    other player fresh %d of 60, deferred %llu\n", other_fresh,
               static_cast<unsigned long long>(h.server().budget_counters().deferred_entries));
  KCD2S_CHECK(h.server().budget_counters().deferred_entries > 0);  // the NPCs did wait
  KCD2S_CHECK(other_fresh >= 58);
}

// Distance rates: a far player's motion reaches a client less often, a near one's every tick, and a
// far player's change that may not wait (health) goes at once.
KCD2S_TEST(interest_budget, far_motion_waits_and_state_goes_at_once) {
  Harness h;
  Peer& viewer = h.add_peer(Vec3{0, 0, 0});
  const EntityId near = h.add_remote(Vec3{10, 0, 0});
  const EntityId far = h.add_remote(Vec3{80, 0, 0});
  h.pump(20);
  KCD2S_CHECK(h.seen(viewer, near) != nullptr);
  KCD2S_CHECK(h.seen(viewer, far) != nullptr);

  int near_fresh = 0;
  int far_fresh = 0;
  for (int t = 0; t < 60; ++t) {
    const float step = static_cast<float>(t + 1) / 8.0f;  // on the wire's 1/256 m grid
    h.move(near, Vec3{10, step, 0});
    h.move(far, Vec3{80, step, 0});
    h.pump(1);
    const auto* near_seen = h.seen(viewer, near);
    const auto* far_seen = h.seen(viewer, far);
    if (near_seen && position_of(*near_seen).y == step) {
      ++near_fresh;
    }
    if (far_seen && position_of(*far_seen).y == step) {
      ++far_fresh;
    }
  }
  std::fprintf(stderr, "    near fresh %d of 60, far fresh %d of 60\n", near_fresh, far_fresh);
  KCD2S_CHECK(near_fresh >= 58);
  KCD2S_CHECK(far_fresh >= 3);  // about 5 Hz at 80 m
  KCD2S_CHECK(far_fresh <= 10);

  // Health is not motion: it reaches the client within the next ticks, not at the next due one.
  h.server().world().write(far, local::Health, 13.0f);
  h.pump(2);
  const auto* far_seen = h.seen(viewer, far);
  KCD2S_CHECK(far_seen && std::get<float>(far_seen->fields[local::Health]) == 13.0f);
}

// A crowd within a few metres: the nearest characters keep full rate, the rest of the crowd goes
// at 10 to 20 Hz instead of everyone at the rate the budget allows.
KCD2S_TEST(interest_budget, a_crowd_keeps_its_nearest_at_full_rate) {
  Harness h;
  Peer& viewer = h.add_peer(Vec3{0, 0, 0});
  std::vector<EntityId> crowd;
  for (int i = 0; i < 100; ++i) {
    // Ring by ring outwards: the first are the nearest.
    const float radius = 1.0f + 0.1f * static_cast<float>(i);
    crowd.push_back(h.add_remote(Vec3{radius, 0, 0}));
  }
  h.pump(30);

  int nearest_fresh = 0;
  int farthest_fresh = 0;
  for (int t = 0; t < 60; ++t) {
    const float step = static_cast<float>(t + 1) / 64.0f;  // on the wire's 1/256 m grid
    for (std::size_t i = 0; i < crowd.size(); ++i) {
      h.move(crowd[i], Vec3{1.0f + 0.1f * static_cast<float>(i), step, 0});
    }
    h.pump(1);
    const auto* nearest = h.seen(viewer, crowd.front());
    const auto* farthest = h.seen(viewer, crowd.back());
    if (nearest && position_of(*nearest).y == step) {
      ++nearest_fresh;
    }
    if (farthest && position_of(*farthest).y == step) {
      ++farthest_fresh;
    }
  }
  std::fprintf(stderr, "    nearest fresh %d of 60, farthest fresh %d of 60\n", nearest_fresh,
               farthest_fresh);
  KCD2S_CHECK(nearest_fresh >= 58);
  KCD2S_CHECK(farthest_fresh >= 6);  // about 10 Hz
  KCD2S_CHECK(farthest_fresh <= 14);
}

KCD2S_TEST(interest_budget, snapshot_build_target_is_what_the_tick_has_left) {
  auto schema = local::player_schema();
  Server server(schema, InterestConfig{64.0f, UpAxis::Z});
  KCD2S_CHECK(server.snapshot_build_target() == 0.008);
  server.set_snapshot_build_target(0.005);
  KCD2S_CHECK(server.snapshot_build_target() == 0.005);
  // Never more than the default, never so little that a crowd spreads at every hiccup.
  server.set_snapshot_build_target(0.02);
  KCD2S_CHECK(server.snapshot_build_target() == 0.008);
  server.set_snapshot_build_target(-0.004);
  KCD2S_CHECK(server.snapshot_build_target() == 0.002);
}

KCD2S_TEST(interest_budget, view_revision_moves_only_when_the_visible_set_changes) {
  Harness h;
  Peer& viewer = h.add_peer(Vec3{0, 0, 0});
  const EntityId other = h.add_remote(Vec3{40, 0, 0});
  KCD2S_CHECK(h.server().view_revision(viewer.id) == 0);  // no view built yet
  h.pump(5);
  const auto first = h.server().view_revision(viewer.id);
  KCD2S_CHECK(first != 0);

  // Moving inside the range keeps the same set, so the same revision.
  h.move(other, Vec3{60, 0, 0});
  h.pump(5);
  KCD2S_CHECK(h.server().view_revision(viewer.id) == first);

  // Leaving and coming back are both new views.
  h.move(other, Vec3{400, 0, 0});
  h.pump(5);
  const auto left = h.server().view_revision(viewer.id);
  KCD2S_CHECK(left != first);
  h.move(other, Vec3{40, 0, 0});
  h.pump(5);
  KCD2S_CHECK(h.server().view_revision(viewer.id) != left);

  const auto owners = h.server().ownership_revision();
  h.server().set_owner(other, viewer.id);
  KCD2S_CHECK(h.server().ownership_revision() != owners);
}
