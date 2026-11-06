#include "../local/movement.hpp"
#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/combat_capabilities.hpp"
#include "kcd2s/integration/server.hpp"
#include "kcd2s/testkit.hpp"
#include "kcd2s/transport/datagram.hpp"

#include <array>
#include <memory>

namespace {
using namespace kcd2s;

struct CapabilitySession {
  struct Peer {
    std::pair<std::unique_ptr<transport::SimLink>, std::unique_ptr<transport::SimLink>> links;
    std::unique_ptr<integration::ClientPeer> client;
    core::EntityId player{};
  };

  replication::SchemaRegistry schema = local::player_schema();
  integration::Server server{schema};
  std::array<Peer, 3> peers;
  unsigned connected = 0;
  double now = 0;

  void pump(unsigned frames = 30) {
    for (unsigned frame = 0; frame < frames; ++frame) {
      now += 1.0 / 60.0;
      server.world().set_tick(server.world().tick() + 1);
      server.tick(now);
      for (unsigned i = 0; i < connected; ++i) {
        peers[i].client->tick(now);
      }
    }
  }

  Peer& join() {
    auto& peer = peers.at(connected++);
    peer.links = transport::make_sim_pair({}, {});
    peer.player = local::spawn_player(server.world());
    const auto id = server.add_client(peer.links.first.get(), peer.player, 1000);
    server.set_owner(peer.player, id);
    server.configure_world(id, {});
    peer.client = std::make_unique<integration::ClientPeer>(schema, peer.links.second.get());
    pump();
    peer.client->mark_world_ready(peer.client->world_config()->revision);
    pump();
    return peer;
  }

  std::int64_t mirrored(const Peer& peer, core::EntityId player) const {
    const auto* entity = peer.client->mirror().get(player);
    return std::get<std::int64_t>(entity->fields.at(local::RiposteAbility));
  }
};
}  // namespace

KCD2S_TEST(combat_capabilities, exact_owned_perk_identity_controls_projection) {
  integration::Progression state;
  KCD2S_CHECK(!integration::owns_riposte_ability(state));
  auto unrelated = integration::kRiposteAbilityPerk;
  unrelated.back() ^= 1;
  state.perks = {unrelated};
  KCD2S_CHECK(!integration::owns_riposte_ability(state));
  state.perks.push_back(integration::kRiposteAbilityPerk);
  integration::normalize_perks(state.perks);
  KCD2S_CHECK(integration::owns_riposte_ability(state));
  KCD2S_CHECK_EQ(integration::perk_id_text(integration::kRiposteAbilityPerk),
                 std::string("ec4c5274-50e3-4bbf-9220-823b080647c4"));
}

KCD2S_TEST(combat_capabilities, server_projection_reaches_late_join_and_explicit_removal) {
  CapabilitySession session;
  auto& owner = session.join();
  auto& observer = session.join();
  const auto* field = session.schema.get("NetworkPlayer")->find("riposteAbility");
  KCD2S_CHECK(field->auth == replication::Authority::Server);
  KCD2S_CHECK_EQ(session.mirrored(observer, owner.player), std::int64_t(-1));

  // Even the actual owner cannot author this field directly. It is a server
  // projection of the separately decoded, associated progression report.
  owner.client->send_input(owner.player, {{local::RiposteAbility, std::int64_t(1)}});
  observer.client->send_input(owner.player, {{local::RiposteAbility, std::int64_t(1)}});
  session.pump();
  KCD2S_CHECK_EQ(session.mirrored(observer, owner.player), std::int64_t(-1));

  integration::Progression progression;
  progression.perks = {integration::kRiposteAbilityPerk};
  KCD2S_CHECK(owner.client->send_progression(progression));
  session.pump();
  KCD2S_CHECK_EQ(session.mirrored(observer, owner.player), std::int64_t(1));
  auto& late = session.join();
  KCD2S_CHECK_EQ(session.mirrored(late, owner.player), std::int64_t(1));

  progression.perks.clear();
  KCD2S_CHECK(owner.client->send_progression(progression));
  session.pump();
  KCD2S_CHECK_EQ(session.mirrored(observer, owner.player), std::int64_t(0));
  KCD2S_CHECK_EQ(session.mirrored(late, owner.player), std::int64_t(0));
}

KCD2S_TEST(combat_capabilities, full_sdk_report_inbox_does_not_block_native_state) {
  CapabilitySession session;
  auto& owner = session.join();
  auto& observer = session.join();
  integration::Progression progression;
  for (unsigned report = 0; report < 256; ++report) {
    progression.main_points = static_cast<std::uint16_t>(report);
    KCD2S_CHECK(owner.client->send_progression(progression));
    session.pump(2);
  }
  session.pump();
  KCD2S_CHECK_EQ(session.mirrored(observer, owner.player), std::int64_t(0));

  progression.perks = {integration::kRiposteAbilityPerk};
  KCD2S_CHECK(owner.client->send_progression(progression));
  session.pump();
  KCD2S_CHECK_EQ(session.mirrored(observer, owner.player), std::int64_t(1));
  KCD2S_CHECK_EQ(session.server.take_progressions().size(), std::size_t(256));
}
