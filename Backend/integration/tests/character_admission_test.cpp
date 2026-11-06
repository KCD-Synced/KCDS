#include "../local/movement.hpp"

#include <algorithm>
#include <memory>
#include <limits>
#include <unordered_set>
#include <vector>

#include "kcd2s/integration/character_admission.hpp"
#include "kcd2s/core/bits.hpp"
#include "kcd2s/integration/horse.hpp"
#include "kcd2s/integration/protocol.hpp"
#include "kcd2s/integration/server.hpp"
#include "kcd2s/testkit.hpp"

using namespace kcd2s;
using namespace kcd2s::integration;

namespace {

bool character(core::TypeId type) {
  return type == core::type_id("NetworkPlayer") || type == core::type_id("NetworkNpc") ||
         type == core::type_id("NetworkHorse");
}

struct AdmissionPeer {
  std::pair<std::unique_ptr<transport::SimLink>, std::unique_ptr<transport::SimLink>> links;
  std::unique_ptr<transport::Connection> connection;
  std::unique_ptr<replication::Mirror> mirror;
  core::PeerId peer = 0;
  core::EntityId avatar = 0;
  bool acknowledge = true;
  std::uint64_t last_constructs = 0;
};

class AdmissionWorld {
 public:
  explicit AdmissionWorld(testkit::Context& context) : kcd2s_ctx(context) {
  }

  testkit::Context& kcd2s_ctx;
  replication::SchemaRegistry schema = local::player_schema();
  Server server{schema, replication::InterestConfig{64.0f, replication::UpAxis::Z}};
  std::vector<std::unique_ptr<AdmissionPeer>> peers;
  double now = 0;

  AdmissionPeer& add_peer(core::Vec3 position = {}, transport::LinkParams loss = {}) {
    auto peer = std::make_unique<AdmissionPeer>();
    peer->links = transport::make_sim_pair(loss, loss);
    peer->avatar = add("NetworkPlayer", position);
    peer->peer = server.add_client(peer->links.first.get(), peer->avatar, 150.0f);
    server.set_owner(peer->avatar, peer->peer);
    peer->connection = std::make_unique<transport::Connection>(peer->links.second.get());
    peer->connection->set_channel_tier(kChannelReplAck,
                                       transport::DeliveryTier::UnreliableSequenced);
    peer->mirror = std::make_unique<replication::Mirror>(schema);
    peers.push_back(std::move(peer));
    return *peers.back();
  }

  core::EntityId add(const char* type, core::Vec3 position) {
    const auto entity = server.world().spawn(core::type_id(type));
    server.world().write(entity, 0, position);
    return entity;
  }

  void move(core::EntityId entity, float x) {
    server.world().write(entity, 0, core::Vec3{x, 0, 0});
  }

  void pump(unsigned ticks) {
    for (unsigned tick = 0; tick < ticks; ++tick) {
      now += 1.0 / 60.0;
      server.world().set_tick(server.world().tick() + 1);
      for (auto& peer : peers) {
        peer->links.first->poll();
        peer->links.second->poll();
      }
      server.tick(now);
      for (auto& peer : peers) {
        for (const auto& message : peer->connection->receive(now)) {
          if (message.channel == static_cast<std::uint8_t>(wire::Channel::Snapshot)) {
            KCD2S_CHECK(peer->mirror->apply(message.bytes));
            core::BitReader header(message.bytes);
            header.read_bits(32);
            header.read_bits(32);
            peer->last_constructs = header.read_varuint();
          }
        }
        if (peer->acknowledge && peer->mirror->has_applied()) {
          peer->connection->queue(kChannelReplAck, encode_tick(peer->mirror->last_applied()));
        }
        peer->connection->flush(now);
        if (server.character_limit()) {
          KCD2S_CHECK(count(*peer) <= server.character_limit());
          KCD2S_CHECK(server.character_admission(peer->peer).resident <= server.character_limit());
        }
      }
    }
  }

  std::size_t count(const AdmissionPeer& peer) const {
    std::size_t result = 0;
    for (const auto& [entity, state] : peer.mirror->entities()) {
      (void)entity;
      if (character(state.type)) {
        ++result;
      }
    }
    return result;
  }

  bool seen(const AdmissionPeer& peer, core::EntityId entity) const {
    return peer.mirror->get(entity) != nullptr;
  }

  void ready() {
    for (auto& peer : peers) {
      KCD2S_CHECK(server.configure_world(peer->peer, {}));
    }
    pump(5);
    for (auto& peer : peers) {
      peer->connection->queue(0, encode_world_ready(1));
      peer->connection->queue(0, encode_gameplay_ready(1));
      peer->connection->flush(now);
    }
    pump(5);
  }
};

}  // namespace

KCD2S_TEST(character_admission, disabled_preserves_all_nearby_character_types) {
  AdmissionWorld world{kcd2s_ctx};
  auto& peer = world.add_peer();
  for (unsigned i = 0; i < 24; ++i) {
    world.add("NetworkPlayer", {float(i), 0, 0});
    world.add("NetworkNpc", {float(i), 1, 0});
    world.add("NetworkHorse", {float(i), 2, 0});
  }
  world.pump(20);
  KCD2S_CHECK_EQ(world.count(peer), std::size_t{73});
  KCD2S_CHECK_EQ(world.server.visible_entities(peer.peer).size(), std::size_t{73});
}

KCD2S_TEST(character_admission, despawned_entity_leaves_the_visible_set_at_once) {
  AdmissionWorld world{kcd2s_ctx};
  auto& peer = world.add_peer();
  const auto stays = world.add("NetworkPlayer", {1, 0, 0});
  const auto leaves = world.add("NetworkPlayer", {2, 0, 0});
  world.pump(20);
  KCD2S_CHECK(world.seen(peer, leaves));
  world.server.despawn(leaves);
  const auto view = world.server.visible_entities(peer.peer);
  KCD2S_CHECK(std::find(view.begin(), view.end(), leaves) == view.end());
  KCD2S_CHECK(std::find(view.begin(), view.end(), stays) != view.end());
}

KCD2S_TEST(character_admission, shared_budget_selects_nearest_across_character_types) {
  AdmissionWorld world{kcd2s_ctx};
  world.server.set_character_limit(4);
  auto& peer = world.add_peer();
  const auto horse = world.add("NetworkHorse", {3, 0, 0});
  const auto npc = world.add("NetworkNpc", {2, 0, 0});
  const auto player = world.add("NetworkPlayer", {1, 0, 0});
  const auto far = world.add("NetworkPlayer", {10, 0, 0});
  const auto prop = world.add("NetworkGroundItem", {4, 0, 0});
  world.pump(20);
  KCD2S_CHECK(world.seen(peer, horse));
  KCD2S_CHECK(world.seen(peer, npc));
  KCD2S_CHECK(world.seen(peer, player));
  KCD2S_CHECK(!world.seen(peer, far));
  KCD2S_CHECK(world.seen(peer, prop));
  const auto view = world.server.visible_entities(peer.peer);
  KCD2S_CHECK(std::find(view.begin(), view.end(), far) == view.end());
  KCD2S_CHECK_EQ(world.server.character_admission(peer.peer).deferred, std::size_t{1});
}

KCD2S_TEST(character_admission, replacement_waits_for_destruct_ack_even_after_world_despawn) {
  AdmissionWorld world{kcd2s_ctx};
  world.server.set_character_limit(2);
  auto& peer = world.add_peer();
  const auto first = world.add("NetworkPlayer", {1, 0, 0});
  const auto next = world.add("NetworkNpc", {2, 0, 0});
  world.pump(10);
  KCD2S_CHECK(world.seen(peer, first));
  peer.acknowledge = false;
  world.pump(3);
  world.server.despawn(first);
  world.pump(20);
  KCD2S_CHECK(!world.seen(peer, first));
  KCD2S_CHECK(!world.seen(peer, next));
  KCD2S_CHECK_EQ(world.server.character_admission(peer.peer).resident, std::size_t{2});
  KCD2S_CHECK_EQ(world.server.character_admission(peer.peer).retiring, std::size_t{1});
  peer.acknowledge = true;
  world.pump(10);
  KCD2S_CHECK(world.seen(peer, next));
  KCD2S_CHECK_EQ(world.server.character_admission(peer.peer).retiring, std::size_t{0});
}

KCD2S_TEST(character_admission, unconfirmed_character_leaves_and_reenters_with_current_state) {
  AdmissionWorld world{kcd2s_ctx};
  world.server.set_character_limit(2);
  auto& peer = world.add_peer();
  peer.acknowledge = false;
  const auto departing = world.add("NetworkPlayer", {1, 0, 0});
  const auto waiting = world.add("NetworkNpc", {2, 0, 0});
  world.pump(3);  // CONSTRUCT arrives, but none of its ACKs reaches the server.
  KCD2S_CHECK(world.seen(peer, departing));
  KCD2S_CHECK(!world.seen(peer, waiting));
  world.move(departing, 500);
  world.pump(3);
  // The borrowed confirmed scope is empty. Sent-but-unconfirmed departures still need DESTRUCT.
  KCD2S_CHECK(!world.seen(peer, departing));
  KCD2S_CHECK(!world.seen(peer, waiting));
  KCD2S_CHECK_EQ(world.server.character_admission(peer.peer).retiring, std::size_t{1});
  peer.acknowledge = true;
  world.pump(10);
  KCD2S_CHECK(world.seen(peer, waiting));
  world.server.world().write(departing, local::Health, 45.0f);
  world.move(departing, 1);
  world.pump(12);
  KCD2S_CHECK(world.seen(peer, departing));
  KCD2S_CHECK(!world.seen(peer, waiting));
  const auto* reentered = peer.mirror->get(departing);
  if (reentered) {
    KCD2S_CHECK_EQ(std::get<float>(reentered->fields[local::Health]), 45.0f);
  }
  KCD2S_CHECK_EQ(world.server.character_admission(peer.peer).retiring, std::size_t{0});
}

KCD2S_TEST(character_admission, pending_window_survives_tick_wrap_after_prolonged_ack_loss) {
  AdmissionWorld world{kcd2s_ctx};
  const auto maximum = std::numeric_limits<core::Tick>::max();
  world.server.world().set_tick(maximum - 340);
  world.server.set_character_limit(2);
  auto& peer = world.add_peer();
  const auto first = world.add("NetworkPlayer", {1, 0, 0});
  const auto next = world.add("NetworkNpc", {2, 0, 0});
  world.pump(10);
  KCD2S_CHECK(world.seen(peer, first));
  // The first ACK is high, not serially newer than a fictitious baseline zero.
  KCD2S_CHECK(peer.mirror->last_baseline() != 0);
  peer.acknowledge = false;
  world.pump(300);
  KCD2S_CHECK_EQ(world.server.world().tick(), maximum - 30);
  world.server.despawn(first);
  world.pump(31);
  KCD2S_CHECK_EQ(peer.mirror->last_applied(), core::Tick{0});
  KCD2S_CHECK(!world.seen(peer, first));
  KCD2S_CHECK(!world.seen(peer, next));
  // The pending window is full of pre-wrap records. ACK zero must still release the
  // old native reservation, and recent post-wrap records must not be trimmed first.
  peer.connection->queue(kChannelReplAck, encode_tick(0));
  peer.connection->flush(world.now);
  world.pump(2);
  KCD2S_CHECK(world.seen(peer, next));
  KCD2S_CHECK_EQ(world.server.character_admission(peer.peer).retiring, std::size_t{0});
  peer.acknowledge = true;
  world.pump(10);
  KCD2S_CHECK_EQ(peer.last_constructs, std::uint64_t{0});
  KCD2S_CHECK_EQ(world.count(peer), std::size_t{2});
}

KCD2S_TEST(character_admission, zero_is_a_valid_first_snapshot_acknowledgement) {
  AdmissionWorld world{kcd2s_ctx};
  world.server.world().set_tick(std::numeric_limits<core::Tick>::max());
  world.server.set_character_limit(2);
  auto& peer = world.add_peer();
  world.add("NetworkNpc", {1, 0, 0});
  peer.acknowledge = false;
  world.pump(1);
  KCD2S_CHECK_EQ(peer.mirror->last_applied(), core::Tick{0});
  KCD2S_CHECK(peer.mirror->has_applied());
  KCD2S_CHECK_EQ(peer.last_constructs, std::uint64_t{2});
  peer.connection->queue(kChannelReplAck, encode_tick(0));
  peer.connection->flush(world.now);
  world.pump(1);
  KCD2S_CHECK_EQ(peer.last_constructs, std::uint64_t{0});
}

KCD2S_TEST(character_admission, retention_margin_stops_distance_boundary_churn) {
  AdmissionWorld world{kcd2s_ctx};
  world.server.set_character_limit(2);
  world.server.set_interest_hysteresis(5.0f);
  auto& peer = world.add_peer();
  const auto first = world.add("NetworkPlayer", {10, 0, 0});
  const auto next = world.add("NetworkPlayer", {11, 0, 0});
  world.pump(10);
  world.move(next, 6);
  world.pump(10);
  KCD2S_CHECK(world.seen(peer, first));
  KCD2S_CHECK(!world.seen(peer, next));
  world.move(next, 4);
  world.pump(10);
  KCD2S_CHECK(!world.seen(peer, first));
  KCD2S_CHECK(world.seen(peer, next));
}

KCD2S_TEST(character_admission, combat_and_owned_characters_have_priority_without_bypassing_cap) {
  AdmissionWorld world{kcd2s_ctx};
  world.server.set_character_limit(3);
  auto& peer = world.add_peer();
  const auto close = world.add("NetworkPlayer", {1, 0, 0});
  const auto opponent = world.add("NetworkPlayer", {20, 0, 0});
  const auto owned = world.add("NetworkHorse", {30, 0, 0});
  const auto excess_owned = world.add("NetworkHorse", {40, 0, 0});
  world.server.set_owner(owned, peer.peer);
  world.server.set_owner(excess_owned, peer.peer);
  world.server.note_combat(peer.avatar, opponent);
  world.pump(10);
  KCD2S_CHECK(world.seen(peer, opponent));
  KCD2S_CHECK(world.seen(peer, owned));
  KCD2S_CHECK(!world.seen(peer, close));
  KCD2S_CHECK(!world.seen(peer, excess_owned));
}

KCD2S_TEST(character_admission, mounted_player_and_horse_are_selected_as_one_group) {
  AdmissionWorld world{kcd2s_ctx};
  world.server.set_character_limit(3);
  auto& peer = world.add_peer();
  const auto rider = world.add("NetworkPlayer", {1, 0, 0});
  const auto horse = world.add("NetworkHorse", {1, 0, 0});
  const auto other = world.add("NetworkPlayer", {2, 0, 0});
  world.server.world().write(horse, HorseRider, std::int64_t(rider));
  world.pump(10);
  KCD2S_CHECK(world.seen(peer, rider));
  KCD2S_CHECK(world.seen(peer, horse));
  KCD2S_CHECK(!world.seen(peer, other));
}

KCD2S_TEST(character_admission, scope_restrictions_override_character_priority) {
  AdmissionWorld world{kcd2s_ctx};
  world.server.set_character_limit(3);
  auto& peer = world.add_peer();
  auto& other_peer = world.add_peer({300, 0, 0});
  const auto hidden = world.add("NetworkNpc", {1, 0, 0});
  const auto different_world = world.add("NetworkPlayer", {2, 0, 0});
  const auto allowed = world.add("NetworkHorse", {3, 0, 0});
  world.server.set_owner(hidden, peer.peer);
  world.server.set_visible_to(hidden, other_peer.avatar);
  world.server.set_dimension(different_world, 2);
  world.server.set_dimension(peer.avatar, 1);
  world.server.set_dimension(allowed, 1);
  world.server.note_combat(peer.avatar, different_world);
  world.pump(10);
  KCD2S_CHECK(!world.seen(peer, hidden));
  KCD2S_CHECK(!world.seen(peer, different_world));
  KCD2S_CHECK(world.seen(peer, allowed));
}

KCD2S_TEST(character_admission, loss_and_dense_churn_never_exceed_resident_budget) {
  AdmissionWorld world{kcd2s_ctx};
  world.server.set_character_limit(8);
  transport::LinkParams loss;
  loss.loss = 0.15;
  loss.duplicate = 0.1;
  loss.min_delay = 1;
  loss.max_delay = 4;
  auto& peer = world.add_peer({}, loss);
  std::vector<core::EntityId> characters;
  for (unsigned i = 0; i < 80; ++i) {
    characters.push_back(world.add(i % 2 ? "NetworkNpc" : "NetworkPlayer", {float(i), 0, 0}));
  }
  world.pump(100);
  for (unsigned round = 0; round < 10; ++round) {
    for (unsigned i = 0; i < characters.size(); ++i) {
      world.move(characters[i], float((i + round * 7) % 80));
    }
    world.pump(30);
  }
  world.pump(120);
  KCD2S_CHECK_EQ(world.count(peer), std::size_t{8});
}

KCD2S_TEST(character_admission, simulators_start_after_admission_and_move_to_an_eligible_holder) {
  AdmissionWorld world{kcd2s_ctx};
  world.server.set_character_limit(3);
  auto& first = world.add_peer();
  auto& second = world.add_peer({80, 0, 0});
  world.ready();
  NpcSpawn spawn;
  spawn.name = "Admission NPC";
  spawn.position = {2, 0, 0};
  spawn.soul = "4A2B6C8D-1E3F-4a5b-8c7d-9e0f1a2b3c4d";
  const auto npc = world.server.spawn_npc(spawn);
  const auto horse = world.add("NetworkHorse", {1, 0, 0});
  world.pump(100);
  KCD2S_CHECK_EQ(std::get<std::int64_t>(*world.server.world().read(npc, NpcSimulator)),
                 std::int64_t(first.avatar));
  KCD2S_CHECK_EQ(std::get<std::int64_t>(*world.server.world().read(horse, HorseSimulator)),
                 std::int64_t(first.avatar));
  // Privacy removes the old simulator's admission. The existing holder becomes simulator;
  // assigning to a client that never received the body would leave its AI with no executor.
  KCD2S_CHECK(world.server.set_visible_to(npc, second.avatar));
  KCD2S_CHECK(world.server.set_visible_to(horse, second.avatar));
  world.pump(100);
  KCD2S_CHECK(!world.seen(first, npc));
  KCD2S_CHECK(!world.seen(first, horse));
  KCD2S_CHECK(world.seen(second, npc));
  KCD2S_CHECK(world.seen(second, horse));
  KCD2S_CHECK_EQ(std::get<std::int64_t>(*world.server.world().read(npc, NpcSimulator)),
                 std::int64_t(second.avatar));
  KCD2S_CHECK_EQ(std::get<std::int64_t>(*world.server.world().read(horse, HorseSimulator)),
                 std::int64_t(second.avatar));
}

KCD2S_TEST(character_admission, ownership_migration_can_admit_a_previously_unselected_character) {
  AdmissionWorld world{kcd2s_ctx};
  world.server.set_character_limit(2);
  auto& first = world.add_peer();
  auto& second = world.add_peer({50, 0, 0});
  const auto owned = world.add("NetworkHorse", {1, 0, 0});
  const auto nearer_second = world.add("NetworkPlayer", {49, 0, 0});
  world.server.set_owner(owned, first.peer);
  world.pump(15);
  KCD2S_CHECK(world.seen(first, owned));
  KCD2S_CHECK(world.seen(second, nearer_second));
  KCD2S_CHECK(!world.seen(second, owned));
  world.server.remove_client(first.peer);
  world.pump(20);
  KCD2S_CHECK_EQ(world.server.world().slot(owned)->owner, second.peer);
  KCD2S_CHECK(world.seen(second, owned));
  KCD2S_CHECK(!world.seen(second, nearer_second));
}

KCD2S_TEST(character_admission, selection_is_stable_when_candidate_order_changes) {
  std::vector<CharacterCandidate> candidates{
      {1, 1, 0, 0}, {4, 4, 0, 10}, {2, 2, 0, 10}, {3, 3, 0, 10}};
  const std::unordered_set<core::EntityId> resident{1};
  const auto first = admit_characters(candidates, resident, 1, 3, 0);
  std::reverse(candidates.begin(), candidates.end());
  const auto second = admit_characters(candidates, resident, 1, 3, 0);
  KCD2S_CHECK(first.entities == second.entities);
  KCD2S_CHECK(first.entities == std::vector<core::EntityId>({1, 2, 3}));
}
