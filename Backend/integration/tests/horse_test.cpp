#include "kcd2s/integration/server.hpp"
#include "kcd2s/integration/client_peer.hpp"
#include "../local/movement.hpp"
#include "kcd2s/testkit.hpp"
#include "kcd2s/core/bits.hpp"
#include "kcd2s/wire/packet.hpp"
#include <limits>
using namespace kcd2s;
using namespace kcd2s::integration;
namespace {
struct HorsePair {
  replication::SchemaRegistry schema = local::player_schema();
  Server server{schema};
  struct Peer {
    std::pair<std::unique_ptr<transport::SimLink>, std::unique_ptr<transport::SimLink>> links;
    std::unique_ptr<ClientPeer> client;
    std::unique_ptr<transport::Connection> raw;
    core::EntityId player{};
    core::PeerId id{};
  } peers[2];
  core::EntityId horse{};
  double now{};
  // `loss` is the share of datagrams the links drop. `with_raw` gives each client a second,
  // hand-driven connection on its link (the raw motion and mount messages of the tests below). The
  // health tests send through the ClientPeer alone: two connections on one link would send
  // packets that disturb each other's sequence numbers.
  explicit HorsePair(double loss = .12, bool with_raw = true, bool native_ready = true) {
    for (auto& p : peers) {
      transport::LinkParams params;
      params.loss = loss;
      params.duplicate = .2;
      params.min_delay = 1;
      params.max_delay = 3;
      p.links = transport::make_sim_pair(params, params);
      p.player = local::spawn_player(server.world());
      p.id = server.add_client(p.links.first.get(), p.player, 1000);
      server.set_owner(p.player, p.id);
      server.configure_world(p.id, {});
      p.client = std::make_unique<ClientPeer>(schema, p.links.second.get());
      if (!with_raw) {
        continue;
      }
      p.raw = std::make_unique<transport::Connection>(p.links.second.get());
      p.raw->set_channel_tier(kHorseMotionChannel, transport::DeliveryTier::UnreliableSequenced);
      p.raw->set_channel_tier(kHorseMountChannel, transport::DeliveryTier::ReliableOrdered);
    }
    horse = server.world().spawn(core::type_id("NetworkHorse"));
    server.world().write(horse, 0, core::Vec3{});
    server.world().write(horse, 1, core::Quat{});
    server.world().write(horse, HorseRider, std::int64_t(0));
    server.initialize_horse(horse, peers[0].player);
    pump(240);
    for (auto& p : peers) {
      p.client->mark_world_ready(1);
      if (native_ready) {
        p.client->mark_gameplay_started(1);
      }
    }
    pump(240);
  }
  void pump(int n) {
    for (int i = 0; i < n; ++i) {
      now += 1. / 60;
      server.world().set_tick(server.world().tick() + 1);
      for (auto& p : peers) {
        p.links.first->poll();
        p.links.second->poll();
      }
      server.tick(now);
      for (auto& p : peers) {
        p.client->tick(now);
        if (p.raw) {
          p.raw->flush(now);
        }
      }
    }
  }
  std::int64_t value(core::FieldId f) {
    return std::get<std::int64_t>(*server.world().read(horse, f));
  }
  HorseState state(unsigned peer = 0) {
    HorseState e;
    e.source = peers[peer].player;
    e.horse = horse;
    e.epoch = std::uint32_t(value(HorseEpoch));
    e.revision = 1;
    e.sequence = 1;
    e.position = {2, 0, 0};
    e.animations = local::encode_animation({});
    return e;
  }
  void send(unsigned peer, const HorseState& e) {
    for (int i = 0; i < 8; ++i) {
      peers[peer].raw->queue(kHorseMotionChannel, encode_horse(e));
      pump(12);
    }
    pump(60);
  }
  void mount(unsigned peer, bool flag, std::uint32_t epoch = 0) {
    HorseMount m{1, epoch ? epoch : std::uint32_t(value(HorseEpoch)), peers[peer].player, horse,
                 flag};
    for (int i = 0; i < 8; ++i) {
      peers[peer].raw->queue(kHorseMountChannel, encode_horse_mount(m));
      pump(12);
    }
    pump(60);
  }
};
}  // namespace
KCD2S_TEST(horse, wire_rejects_invalid_native_values) {
  HorseState s;
  s.source = 1;
  s.horse = 2;
  s.sequence = 3;
  s.animations = local::encode_animation({});
  auto b = encode_horse(s);
  HorseState e;
  KCD2S_CHECK(decode_horse(b, e));
  KCD2S_CHECK(e.sequence == 3);
  b.pop_back();
  KCD2S_CHECK(!decode_horse(b, e));
  s.position.x = std::numeric_limits<float>::quiet_NaN();
  KCD2S_CHECK(!decode_horse(encode_horse(s), e));
  s.position = {};
  s.rotation = {0, 0, 0, 0};
  KCD2S_CHECK(!valid_horse(s));
}

KCD2S_TEST(horse, world_only_peers_cannot_be_simulators_or_mount_until_gameplay_starts) {
  HorsePair p(0, false, false);
  KCD2S_CHECK_EQ(p.server.character_limit(), 0u);
  KCD2S_CHECK_EQ(p.value(HorseSimulator), std::int64_t{0});
  KCD2S_CHECK(!p.server.initialize_horse(p.horse, p.peers[0].player));
  KCD2S_CHECK(p.peers[0].client->send_horse_mount(p.peers[0].player, p.horse, true));
  p.pump(30);
  KCD2S_CHECK_EQ(p.value(HorseRider), std::int64_t{0});
  KCD2S_CHECK_EQ(p.value(HorseSimulator), std::int64_t{0});
  KCD2S_CHECK(p.peers[1].client->mark_gameplay_started(1));
  p.pump(30);
  KCD2S_CHECK_EQ(p.value(HorseSimulator), std::int64_t(p.peers[1].player));
  // A desired owner who is still loading cannot take AI. Once their gameplay starts, ownership
  // takes effect without requiring the resource to issue giveTo a second time.
  p.server.world().write(p.horse, HorseOwner, std::int64_t(p.peers[0].player));
  p.pump(30);
  KCD2S_CHECK_EQ(p.value(HorseSimulator), std::int64_t(p.peers[1].player));
  KCD2S_CHECK(p.peers[0].client->mark_gameplay_started(1));
  p.pump(30);
  KCD2S_CHECK_EQ(p.value(HorseSimulator), std::int64_t(p.peers[0].player));
}

KCD2S_TEST(horse, mounted_rider_reloading_yields_until_its_new_gameplay_revision_is_ready) {
  HorsePair p(0, false);
  p.server.world().write(p.horse, HorseOwner, std::int64_t(p.peers[0].player));
  KCD2S_CHECK(p.peers[1].client->send_horse_mount(p.peers[1].player, p.horse, true));
  p.pump(30);
  KCD2S_CHECK_EQ(p.value(HorseRider), std::int64_t(p.peers[1].player));
  KCD2S_CHECK_EQ(p.value(HorseSimulator), std::int64_t(p.peers[1].player));
  const auto before = p.value(HorseEpoch);
  WorldConfig reload;
  reload.revision = 2;
  KCD2S_CHECK(p.server.configure_world(p.peers[1].id, reload));
  p.pump(30);
  KCD2S_CHECK_EQ(p.value(HorseSimulator), std::int64_t{0});
  KCD2S_CHECK_EQ(p.value(HorseRider), std::int64_t(p.peers[1].player));
  KCD2S_CHECK(p.value(HorseEpoch) > before);
  KCD2S_CHECK(p.peers[1].client->mark_world_ready(2));
  p.pump(30);
  KCD2S_CHECK_EQ(p.value(HorseSimulator), std::int64_t{0});
  KCD2S_CHECK(!p.server.initialize_horse(p.horse, p.peers[1].player));
  KCD2S_CHECK(p.peers[1].client->mark_gameplay_started(2));
  p.pump(30);
  KCD2S_CHECK_EQ(p.value(HorseSimulator), std::int64_t(p.peers[1].player));
  KCD2S_CHECK_EQ(p.value(HorseRider), std::int64_t(p.peers[1].player));
  // Departure clears the rider and returns native simulation to the ready owner.
  p.server.remove_client(p.peers[1].id);
  KCD2S_CHECK_EQ(p.value(HorseRider), std::int64_t{0});
  KCD2S_CHECK_EQ(p.value(HorseSimulator), std::int64_t(p.peers[0].player));
}
KCD2S_TEST(horse, idle_publication_does_not_weaken_sequence_or_epoch_fences) {
  HorsePair p(0, false);
  auto state = p.state();
  std::uint16_t sequence = 0;
  const auto send_once = [&](const HorseState& value) {
    // Inject one independently numbered datagram, without a second Connection reusing the
    // real client's packet sequence. This tests the server past the client's publication gate.
    const auto message = encode_horse(value);
    core::BitWriter block;
    block.write_bits(1, 16);
    block.write_bits(++sequence, 16);
    block.write_bits(message.size(), 16);
    block.write_bytes(message.data(), message.size());
    wire::Packet packet;
    packet.protocolId = wire::kProtocolId;
    packet.peerSeq = static_cast<std::uint16_t>(60000 + sequence);
    packet.channels.push_back({kHorseMotionChannel, block.bytes()});
    const auto bytes = wire::write_packet(packet);
    p.peers[0].links.second->send(bytes.data(), bytes.size());
    p.pump(6);
  };
  send_once(state);
  const auto first = std::get<std::string>(*p.server.world().read(p.horse, HorseNativeState));
  KCD2S_CHECK(!first.empty());
  state.sequence = 10;
  send_once(state);
  KCD2S_CHECK(std::get<std::string>(*p.server.world().read(p.horse, HorseNativeState)) == first);
  state.sequence = 9;
  state.position.x = 8;
  send_once(state);
  KCD2S_CHECK(std::get<core::Vec3>(*p.server.world().read(p.horse, HorsePosition)).x == 2);
  state.sequence = 11;
  ++state.epoch;
  send_once(state);
  KCD2S_CHECK(std::get<core::Vec3>(*p.server.world().read(p.horse, HorsePosition)).x == 2);
  --state.epoch;
  send_once(state);
  KCD2S_CHECK(std::get<core::Vec3>(*p.server.world().read(p.horse, HorsePosition)).x == 8);
}

KCD2S_TEST(horse, native_state_replication_and_authority_fence) {
  HorsePair p;
  auto s = p.state();
  p.send(0, s);
  KCD2S_CHECK(std::get<core::Vec3>(*p.server.world().read(p.horse, 0)).x == 2);
  auto other = p.peers[1].client->mirror().get(p.horse);
  KCD2S_CHECK(other && std::get<core::Vec3>(other->fields[0]).x == 2);
  auto forged = p.state(1);
  forged.position.x = 5;
  p.send(1, forged);
  KCD2S_CHECK(std::get<core::Vec3>(*p.server.world().read(p.horse, 0)).x == 2);
  s.position.x = 6;
  p.send(0, s);
  KCD2S_CHECK(std::get<core::Vec3>(*p.server.world().read(p.horse, 0)).x == 2);
  s.sequence = 2;
  s.revision = 2;
  p.send(0, s);
  KCD2S_CHECK(std::get<core::Vec3>(*p.server.world().read(p.horse, 0)).x == 2);
  p.mount(1, true);
  KCD2S_CHECK(p.value(HorseRider) == p.peers[1].player);
  KCD2S_CHECK(p.value(HorseSimulator) == p.peers[1].player);
  auto old_epoch = s.epoch;
  s.sequence = 3;
  s.revision = 1;
  p.send(0, s);
  KCD2S_CHECK(std::get<core::Vec3>(*p.server.world().read(p.horse, 0)).x == 2);
  p.mount(0, true);
  KCD2S_CHECK(p.value(HorseRider) == p.peers[1].player);
  p.mount(0, false);
  KCD2S_CHECK(p.value(HorseRider) == p.peers[1].player);
  p.mount(1, false);
  KCD2S_CHECK(p.value(HorseRider) == 0);
  KCD2S_CHECK(p.value(HorseEpoch) > old_epoch);
  auto newest = p.state(1);
  newest.position.x = 7;
  p.send(1, newest);
  KCD2S_CHECK(std::get<core::Vec3>(*p.server.world().read(p.horse, 0)).x == 7);
  p.server.remove_client(p.peers[1].id);
  KCD2S_CHECK(p.value(HorseRider) == 0);
  KCD2S_CHECK(p.value(HorseSimulator) == p.peers[0].player);
  p.send(1, newest);
  KCD2S_CHECK(std::get<core::Vec3>(*p.server.world().read(p.horse, 0)).x == 7);
}
namespace {
// The helpers of the health tests: a horse at full health, as a resource's horse starts, what the
// server's world holds of its health, and a report through the ClientPeer API of peer `peer`.
void start_alive(HorsePair& p) {
  p.server.set_horse_health(p.horse, kHorseMaxHealth, kHorseAlive);
  p.pump(30);
}
float horse_health(HorsePair& p) {
  return std::get<float>(*p.server.world().read(p.horse, HorseHealth));
}
bool report_health(HorsePair& p, unsigned peer, float health, std::uint8_t state) {
  const bool sent = p.peers[peer].client->send_horse_health(
      p.horse, std::uint32_t(p.value(HorseEpoch)), health, state);
  p.pump(60);
  return sent;
}
}  // namespace
KCD2S_TEST(horse, health_wire_round_trip_and_strict_decode) {
  HorseHealthReport report;
  report.horse = 77;
  report.epoch = 5;
  report.health = 42.5f;
  report.state = std::uint8_t(kHorseAlive);
  const auto bytes = encode_horse_health(report);
  KCD2S_CHECK(bytes.size() == kHorseHealthBytes);
  KCD2S_CHECK(bytes[0] == kHorseHealthTag);
  // The layout: little-endian horse, epoch and float bits, then the state.
  KCD2S_CHECK(bytes[1] == 77 && bytes[2] == 0 && bytes[5] == 5);
  KCD2S_CHECK(bytes[13] == kHorseAlive);
  HorseHealthReport decoded;
  KCD2S_CHECK(decode_horse_health(bytes, decoded));
  KCD2S_CHECK(decoded == report);
  // A lethal blow leaves no health: the dead and the knocked out may report 0.
  report.health = 0;
  report.state = std::uint8_t(kHorseDead);
  KCD2S_CHECK(decode_horse_health(encode_horse_health(report), decoded));
  KCD2S_CHECK(decoded == report);
  report.state = std::uint8_t(kHorseKnockedOut);
  KCD2S_CHECK(decode_horse_health(encode_horse_health(report), decoded));

  // Encoding refuses what decoding would.
  HorseHealthReport bad = report;
  bad.horse = 0;
  KCD2S_CHECK(encode_horse_health(bad).empty());
  bad = report;
  bad.state = std::uint8_t(kHorseAlive);  // alive with 0 health
  KCD2S_CHECK(encode_horse_health(bad).empty());
  bad.health = kHorseMaxHealth + 1;
  KCD2S_CHECK(encode_horse_health(bad).empty());
  bad.health = -1;
  KCD2S_CHECK(encode_horse_health(bad).empty());
  bad.health = std::numeric_limits<float>::quiet_NaN();
  KCD2S_CHECK(encode_horse_health(bad).empty());
  bad = report;
  bad.state = 3;
  KCD2S_CHECK(encode_horse_health(bad).empty());

  // Decoding is strict about the bytes themselves.
  report.health = 10;
  report.state = std::uint8_t(kHorseAlive);
  const auto good = encode_horse_health(report);
  auto patched = [&](auto change) {
    auto copy = good;
    change(copy);
    return decode_horse_health(copy, decoded);
  };
  KCD2S_CHECK(patched([](auto&) {}));
  KCD2S_CHECK(!patched([](auto& b) { b.pop_back(); }));
  KCD2S_CHECK(!patched([](auto& b) { b.push_back(0); }));
  KCD2S_CHECK(!patched([](auto& b) { b[0] = kHorseHealthTag + 1; }));
  KCD2S_CHECK(!patched([](auto& b) { b[13] = 3; }));
  KCD2S_CHECK(!patched([](auto& b) { b[1] = b[2] = b[3] = b[4] = 0; }));
  // Health bits of NaN, of 130 and of -1, then of 0 with the state alive.
  KCD2S_CHECK(!patched([](auto& b) { b[9] = 0, b[10] = 0, b[11] = 0xc0, b[12] = 0x7f; }));
  KCD2S_CHECK(!patched([](auto& b) { b[9] = 0, b[10] = 0, b[11] = 0x02, b[12] = 0x43; }));
  KCD2S_CHECK(!patched([](auto& b) { b[9] = 0, b[10] = 0, b[11] = 0x80, b[12] = 0xbf; }));
  KCD2S_CHECK(!patched([](auto& b) { b[9] = b[10] = b[11] = b[12] = 0; }));
}
KCD2S_TEST(horse, health_taken_only_from_the_simulator_in_the_current_epoch) {
  HorsePair p(0, false);
  start_alive(p);
  KCD2S_CHECK(horse_health(p) == kHorseMaxHealth);
  KCD2S_CHECK(p.value(HorseCondition) == kHorseAlive);
  KCD2S_CHECK(p.value(HorseLife) == 0);
  p.server.take_horse_health_changes();

  // Peer 1 holds the horse but does not simulate it.
  KCD2S_CHECK(report_health(p, 1, 10, kHorseAlive));
  KCD2S_CHECK(horse_health(p) == kHorseMaxHealth);
  KCD2S_CHECK(p.server.take_horse_health_changes().empty());
  // The simulator, in an epoch that is not the current one.
  KCD2S_CHECK(p.peers[0].client->send_horse_health(p.horse, std::uint32_t(p.value(HorseEpoch)) + 1,
                                                   10, kHorseAlive));
  p.pump(90);
  KCD2S_CHECK(horse_health(p) == kHorseMaxHealth);
  KCD2S_CHECK(p.server.take_horse_health_changes().empty());
  // The simulator in the current epoch: the fields change, every client shows them, and the
  // resource host hears of it once.
  KCD2S_CHECK(report_health(p, 0, 60, kHorseAlive));
  KCD2S_CHECK(horse_health(p) == 60);
  KCD2S_CHECK(p.value(HorseCondition) == kHorseAlive);
  const auto* seen = p.peers[1].client->mirror().get(p.horse);
  KCD2S_CHECK(seen && std::get<float>(seen->fields[HorseHealth]) == 60);
  auto changes = p.server.take_horse_health_changes();
  KCD2S_CHECK(changes.size() == 1);
  KCD2S_CHECK(changes[0].horse == p.horse);
  KCD2S_CHECK(changes[0].previous_health == kHorseMaxHealth && changes[0].health == 60);
  KCD2S_CHECK(changes[0].previous_state == kHorseAlive && changes[0].state == kHorseAlive);
  // The same report again changes nothing and is not told.
  KCD2S_CHECK(report_health(p, 0, 60, kHorseAlive));
  KCD2S_CHECK(p.server.take_horse_health_changes().empty());
  // A new simulator starts a new epoch (a rider took the horse): the old simulator's reports are
  // no longer taken.
  const auto old_epoch = std::uint32_t(p.value(HorseEpoch));
  KCD2S_CHECK(p.server.initialize_horse(p.horse, p.peers[1].player));
  KCD2S_CHECK(p.value(HorseSimulator) == p.peers[1].player);
  KCD2S_CHECK(p.peers[0].client->send_horse_health(p.horse, old_epoch, 5, kHorseAlive));
  p.pump(90);
  KCD2S_CHECK(horse_health(p) == 60);
  KCD2S_CHECK(report_health(p, 1, 30, kHorseAlive));
  KCD2S_CHECK(horse_health(p) == 30);
}
KCD2S_TEST(horse, health_death_is_a_knock_out_with_the_option_and_the_dead_stay_dead) {
  HorsePair p(0, false);
  start_alive(p);
  p.server.take_horse_health_changes();

  // Without the option, the death is taken: health 0, dead.
  KCD2S_CHECK(report_health(p, 0, 0, kHorseDead));
  KCD2S_CHECK(horse_health(p) == 0);
  KCD2S_CHECK(p.value(HorseCondition) == kHorseDead);
  auto changes = p.server.take_horse_health_changes();
  KCD2S_CHECK(changes.size() == 1);
  KCD2S_CHECK(changes[0].state == kHorseDead);
  // A report never brings a dead horse back, however much health or which state it names.
  p.pump(90);
  KCD2S_CHECK(report_health(p, 0, 80, kHorseAlive));
  KCD2S_CHECK(report_health(p, 0, 80, kHorseKnockedOut));
  KCD2S_CHECK(horse_health(p) == 0);
  KCD2S_CHECK(p.value(HorseCondition) == kHorseDead);
  KCD2S_CHECK(p.value(HorseLife) == 0);
  KCD2S_CHECK(p.server.take_horse_health_changes().empty());
  // Only the resource brings it back: the life goes up, so every client rebuilds the body.
  KCD2S_CHECK(p.server.set_horse_health(p.horse, 100, kHorseAlive));
  KCD2S_CHECK(p.value(HorseCondition) == kHorseAlive);
  KCD2S_CHECK(p.value(HorseLife) == 1);
  KCD2S_CHECK(horse_health(p) == 100);

  // With the option, a death leaves the horse knocked out.
  p.server.world().write(p.horse, HorseKnockOut, true);
  p.pump(90);
  KCD2S_CHECK(report_health(p, 0, 0, kHorseDead));
  KCD2S_CHECK(p.value(HorseCondition) == kHorseKnockedOut);
  KCD2S_CHECK(horse_health(p) == 0);
  changes = p.server.take_horse_health_changes();
  KCD2S_CHECK(changes.size() == 1);
  KCD2S_CHECK(changes[0].previous_state == kHorseAlive && changes[0].state == kHorseKnockedOut);
  // A knocked-out horse does not get up by a report, and a second lethal blow leaves it as it is.
  p.pump(90);
  KCD2S_CHECK(report_health(p, 0, 50, kHorseAlive));
  KCD2S_CHECK(report_health(p, 0, 0, kHorseDead));
  KCD2S_CHECK(p.value(HorseCondition) == kHorseKnockedOut);
  KCD2S_CHECK(p.server.take_horse_health_changes().empty());
  // Without the option, a knocked-out horse can still die.
  p.server.world().write(p.horse, HorseKnockOut, false);
  p.pump(90);
  KCD2S_CHECK(report_health(p, 0, 0, kHorseDead));
  KCD2S_CHECK(p.value(HorseCondition) == kHorseDead);
  KCD2S_CHECK(p.server.take_horse_health_changes().size() == 1);
}
KCD2S_TEST(horse, health_resource_decisions_and_rate_limit) {
  HorsePair p(0, false);
  KCD2S_CHECK(p.server.set_horse_health(p.horse, 100, kHorseAlive));
  // Only a wake or revive (back to alive from a state that lies) raises the life.
  KCD2S_CHECK(p.server.set_horse_health(p.horse, 40, kHorseAlive));
  KCD2S_CHECK(p.value(HorseLife) == 0);
  KCD2S_CHECK(p.server.set_horse_health(p.horse, 0, kHorseKnockedOut));
  KCD2S_CHECK(p.value(HorseCondition) == kHorseKnockedOut);
  KCD2S_CHECK(p.value(HorseLife) == 0);
  KCD2S_CHECK(p.server.set_horse_health(p.horse, 55, kHorseAlive));
  KCD2S_CHECK(p.value(HorseLife) == 1);
  KCD2S_CHECK(p.server.set_horse_health(p.horse, 0, kHorseDead));
  KCD2S_CHECK(p.server.set_horse_health(p.horse, 100, kHorseAlive));
  KCD2S_CHECK(p.value(HorseLife) == 2);
  // Refused: alive with no health, a health out of range, an unknown state, an unknown horse and
  // an entity that is no horse.
  KCD2S_CHECK(!p.server.set_horse_health(p.horse, 0, kHorseAlive));
  KCD2S_CHECK(!p.server.set_horse_health(p.horse, -1, kHorseDead));
  KCD2S_CHECK(!p.server.set_horse_health(p.horse, kHorseMaxHealth + 1, kHorseAlive));
  KCD2S_CHECK(!p.server.set_horse_health(p.horse, std::numeric_limits<float>::quiet_NaN(),
                                         kHorseKnockedOut));
  KCD2S_CHECK(!p.server.set_horse_health(p.horse, 10, 3));
  KCD2S_CHECK(!p.server.set_horse_health(p.horse, 10, -1));
  KCD2S_CHECK(!p.server.set_horse_health(p.horse + 1000, 10, kHorseAlive));
  KCD2S_CHECK(!p.server.set_horse_health(p.peers[0].player, 10, kHorseAlive));
  KCD2S_CHECK(horse_health(p) == 100);
  KCD2S_CHECK(p.value(HorseLife) == 2);

  // At most kMaxHorseHealthsPerSecond reports of a horse are taken in a second.
  p.pump(60);
  p.server.take_horse_health_changes();
  const auto epoch = std::uint32_t(p.value(HorseEpoch));
  for (int i = 1; i <= 8; ++i) {
    KCD2S_CHECK(p.peers[0].client->send_horse_health(p.horse, epoch, 100.0f - 10 * i, kHorseAlive));
  }
  // The eight travel together, so they reach the server within the same second.
  p.pump(120);
  KCD2S_CHECK(p.server.take_horse_health_changes().size() == kMaxHorseHealthsPerSecond);
  // The next second takes more.
  p.pump(90);
  KCD2S_CHECK(report_health(p, 0, 5, kHorseAlive));
  KCD2S_CHECK(horse_health(p) == 5);
}

KCD2S_TEST(horse, stamina_wire_round_trip_and_strict_decode) {
  HorseStaminaReport report;
  report.horse = 77;
  report.epoch = 5;
  report.stamina = 83.5f;
  const auto bytes = encode_horse_stamina(report);
  KCD2S_CHECK(bytes.size() == kHorseStaminaBytes);
  KCD2S_CHECK(bytes[0] == kHorseStaminaTag);
  HorseStaminaReport decoded;
  KCD2S_CHECK(decode_horse_stamina(bytes, decoded));
  KCD2S_CHECK(decoded == report);
  // Out of range, no horse, or another length: nothing.
  report.stamina = -1;
  KCD2S_CHECK(encode_horse_stamina(report).empty());
  report.stamina = kHorseMaxStaminaReport + 1;
  KCD2S_CHECK(encode_horse_stamina(report).empty());
  report.stamina = 10;
  report.horse = 0;
  KCD2S_CHECK(encode_horse_stamina(report).empty());
  auto longer = bytes;
  longer.push_back(0);
  KCD2S_CHECK(!decode_horse_stamina(longer, decoded));
}

KCD2S_TEST(horse, stamina_taken_only_from_the_simulator_in_the_current_epoch_and_rate_limited) {
  HorsePair p(0, false);
  start_alive(p);
  p.server.take_horse_stamina_changes();
  const auto epoch = std::uint32_t(p.value(HorseEpoch));
  // Peer 1 holds the horse but does not simulate it; the simulator in another epoch.
  KCD2S_CHECK(p.peers[1].client->send_horse_stamina(p.horse, epoch, 40));
  KCD2S_CHECK(p.peers[0].client->send_horse_stamina(p.horse, epoch + 1, 40));
  p.pump(60);
  KCD2S_CHECK(p.server.take_horse_stamina_changes().empty());
  // The simulator in the current epoch: taken, at most kMaxHorseStaminasPerSecond a second.
  for (int i = 0; i < 5; ++i) {
    KCD2S_CHECK(p.peers[0].client->send_horse_stamina(p.horse, epoch, 50.0f - float(i)));
  }
  p.pump(10);
  const auto taken = p.server.take_horse_stamina_changes();
  KCD2S_CHECK_EQ(taken.size(), std::size_t(kMaxHorseStaminasPerSecond));
  KCD2S_CHECK(taken[0].horse == p.horse && taken[0].stamina == 50.0f);
}
