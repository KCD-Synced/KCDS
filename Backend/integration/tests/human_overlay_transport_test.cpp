#include "../local/human_overlay.hpp"
#include "../local/movement.hpp"
#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/server.hpp"
#include "kcd2s/testkit.hpp"
#include "kcd2s/transport/datagram.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace kcd2s;

struct OverlaySession {
  struct Peer {
    std::pair<std::unique_ptr<transport::SimLink>, std::unique_ptr<transport::SimLink>> links;
    std::unique_ptr<integration::ClientPeer> client;
    core::EntityId player{};
    core::PeerId id{};
  };

  replication::SchemaRegistry schema = local::player_schema();
  integration::Server server{schema};
  std::array<Peer, 3> peers;
  testkit::Context& kcd2s_ctx;
  unsigned connected = 0;
  double now = 0;

  explicit OverlaySession(testkit::Context& context) : kcd2s_ctx(context) {
  }

  void pump(unsigned frames = 12) {
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
    if (connected >= peers.size()) {
      throw std::runtime_error("overlay fixture peer capacity exceeded");
    }
    auto& peer = peers[connected++];
    peer.links = transport::make_sim_pair({}, {});
    peer.player = local::spawn_player(server.world());
    peer.id = server.add_client(peer.links.first.get(), peer.player, 1000);
    server.set_owner(peer.player, peer.id);
    KCD2S_CHECK(server.configure_world(peer.id, {}));
    peer.client = std::make_unique<integration::ClientPeer>(schema, peer.links.second.get());
    pump();
    if (!peer.client->world_config()) {
      throw std::runtime_error("overlay peer did not receive world configuration");
    }
    KCD2S_CHECK(peer.client->mark_world_ready(peer.client->world_config()->revision));
    pump();
    KCD2S_CHECK(peer.client->owns(peer.player));
    return peer;
  }

  void publish(Peer& peer, core::EntityId player, const local::HumanOverlay& overlay) {
    const auto payload = local::encode_human_overlay(overlay);
    KCD2S_CHECK_EQ(payload.size(), local::kHumanOverlayWireBytes);
    peer.client->send_input(player, {{local::HumanOverlayAnimation, payload}});
    pump();
  }

  std::string authoritative(core::EntityId player, core::FieldId field) {
    const auto value = server.world().read(player, field);
    if (!value || !std::holds_alternative<std::string>(*value)) {
      throw std::runtime_error("missing authoritative overlay fixture string field");
    }
    return std::get<std::string>(*value);
  }

  std::string mirrored(const Peer& peer, core::EntityId player, core::FieldId field) const {
    const auto* entity = peer.client->mirror().get(player);
    if (!entity || field >= entity->fields.size() ||
        !std::holds_alternative<std::string>(entity->fields[field])) {
      throw std::runtime_error("missing mirrored overlay fixture string field");
    }
    return std::get<std::string>(entity->fields[field]);
  }

  void check_overlay(const Peer& replica, core::EntityId player,
                     const local::HumanOverlay& expected) {
    const auto payload = local::encode_human_overlay(expected);
    KCD2S_CHECK_EQ(authoritative(player, local::HumanOverlayAnimation), payload);
    const auto received = mirrored(replica, player, local::HumanOverlayAnimation);
    KCD2S_CHECK_EQ(received, payload);
    const auto decoded = local::decode_human_overlay(received);
    KCD2S_CHECK(decoded);
    if (!decoded) {
      return;
    }
    KCD2S_CHECK_EQ(decoded->sequence, expected.sequence);
    KCD2S_CHECK_EQ(decoded->alias_hash, expected.alias_hash);
    KCD2S_CHECK(decoded->state == expected.state);
  }
};

local::HumanOverlay started() {
  local::HumanOverlay overlay;
  overlay.sequence = 0x1234567800000001ULL;
  overlay.alias_hash = 0xF02512BEC7816746ULL;
  overlay.state = local::HumanOverlayState::Start;
  overlay.params.transition = 0.25f;
  overlay.params.normalized_time = 0.75f;
  overlay.params.speed = 1.5f;
  overlay.params.weight = 0.5f;
  overlay.params.flags = 0x40000004;
  overlay.params.token = 0xFFEEDDCC;
  overlay.params.user_token = 0xAABBCCDD;
  for (unsigned i = 0; i < 8; ++i) {
    overlay.params.user_data[i] = float(i) - 3.5f;
  }
  return overlay;
}

}  // namespace

KCD2S_TEST(human_overlay_transport, owner_state_reaches_two_replicas_including_late_join) {
  OverlaySession session(kcd2s_ctx);
  auto& owner = session.join();
  auto& first = session.join();
  const auto* player_type = session.schema.get("NetworkPlayer");
  KCD2S_CHECK(player_type);
  if (!player_type) {
    return;
  }
  const auto* field = player_type->find("humanOverlay");
  KCD2S_CHECK(field);
  if (!field) {
    return;
  }
  KCD2S_CHECK_EQ(field->id, local::HumanOverlayAnimation);
  KCD2S_CHECK(field->kind == replication::FieldKind::Str);
  KCD2S_CHECK(field->auth == replication::Authority::Owner);
  KCD2S_CHECK(field->prio == replication::Priority::High);

  const auto body = session.authoritative(owner.player, local::Animations);
  KCD2S_CHECK(session.authoritative(owner.player, local::HumanOverlayAnimation).empty());
  KCD2S_CHECK(session.mirrored(first, owner.player, local::HumanOverlayAnimation).empty());
  auto overlay = started();
  session.publish(owner, owner.player, overlay);
  session.check_overlay(first, owner.player, overlay);

  // A late join receives the current alias and all 64 parameter bytes from the server snapshot;
  // the owner does not send the field again and no process-local animation index is supplied.
  auto& late = session.join();
  session.check_overlay(first, owner.player, overlay);
  session.check_overlay(late, owner.player, overlay);
  KCD2S_CHECK_EQ(session.mirrored(first, owner.player, local::Animations), body);
  KCD2S_CHECK_EQ(session.mirrored(late, owner.player, local::Animations), body);

  overlay.sequence++;
  overlay.state = local::HumanOverlayState::Stop;
  overlay.alias_hash = 0;
  session.publish(owner, owner.player, overlay);
  session.check_overlay(first, owner.player, overlay);
  session.check_overlay(late, owner.player, overlay);

  const auto stopped_sequence = overlay.sequence;
  overlay = started();
  overlay.sequence = stopped_sequence + 1;
  overlay.params.normalized_time = 0;
  session.publish(owner, owner.player, overlay);
  session.check_overlay(first, owner.player, overlay);
  session.check_overlay(late, owner.player, overlay);
  KCD2S_CHECK_EQ(session.authoritative(owner.player, local::Animations), body);
}

KCD2S_TEST(human_overlay_transport, server_rejects_owner_forging_another_players_field) {
  OverlaySession session(kcd2s_ctx);
  auto& owner = session.join();
  auto& other = session.join();
  auto& observer = session.join();
  const auto original = started();
  session.publish(owner, owner.player, original);
  session.check_overlay(other, owner.player, original);
  session.check_overlay(observer, owner.player, original);

  auto forged = original;
  forged.sequence += 100;
  forged.alias_hash = 0x8000000000000001ULL;
  KCD2S_CHECK(!other.client->owns(owner.player));
  KCD2S_CHECK(!owner.client->owns(other.player));
  KCD2S_CHECK(other.client->mirror().get(owner.player));
  KCD2S_CHECK(owner.client->mirror().get(other.player));

  // ClientPeer encodes visible non-owned entities, so these requests reach the server's
  // authority check rather than being refused by a local owner-only convenience wrapper.
  session.publish(other, owner.player, forged);
  session.check_overlay(other, owner.player, original);
  session.check_overlay(observer, owner.player, original);

  session.publish(owner, other.player, forged);
  KCD2S_CHECK(session.authoritative(other.player, local::HumanOverlayAnimation).empty());
  KCD2S_CHECK(session.mirrored(owner, other.player, local::HumanOverlayAnimation).empty());
  KCD2S_CHECK(session.mirrored(observer, other.player, local::HumanOverlayAnimation).empty());

  // Rejection does not block a subsequent write from the actual owner of that player.
  session.publish(other, other.player, forged);
  session.check_overlay(owner, other.player, forged);
  session.check_overlay(observer, other.player, forged);
  session.check_overlay(observer, owner.player, original);
}
