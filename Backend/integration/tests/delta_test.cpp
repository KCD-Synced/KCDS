// Changed-field deltas under repeated owner input (phase 1).
//
// Owner clients re-send every AUTH_OWNER field on every input, including the large animation
// state. The server must stamp a field as changed only when its encoded value differs, so a
// steady-state snapshot carries no update for an owner whose state did not change, a real change
// still reaches every observer, and a late joiner still receives the full current state.
#include "../local/movement.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/server.hpp"
#include "kcd2s/replication/schema.hpp"
#include "kcd2s/replication/world.hpp"
#include "kcd2s/testkit.hpp"
#include "kcd2s/transport/datagram.hpp"

using namespace kcd2s::integration;
using kcd2s::core::EntityId;
using kcd2s::core::FieldId;
using kcd2s::core::Quat;
using kcd2s::core::Tick;
using kcd2s::core::Vec3;
using kcd2s::replication::Authority;
using kcd2s::replication::FieldValue;
using kcd2s::replication::SchemaRegistry;
using kcd2s::transport::make_loopback_pair;
using kcd2s::transport::SimLink;

namespace {

// Size of an empty snapshot: u32 snapTick + u32 baselineTick + three zero varuint counts.
constexpr std::uint64_t kEmptySnapshotBytes = 11;

// The game client's animation payload is 1220 bytes; the rider state is 22 bytes; an outfit of
// five pieces is 83 bytes.
constexpr std::size_t kAnimationBytes = 1220;
constexpr std::size_t kRiderStateBytes = 22;
constexpr std::size_t kOutfitBytes = 83;

using Input = std::vector<std::pair<FieldId, FieldValue>>;

struct Peer {
  std::unique_ptr<SimLink> server_side;
  std::unique_ptr<SimLink> client_side;
  std::unique_ptr<ClientPeer> peer;
  PeerId id = 0;
  EntityId avatar = 0;
};

Peer join(Server& server, const SchemaRegistry& schema) {
  Peer p;
  auto links = make_loopback_pair();
  p.server_side = std::move(links.first);
  p.client_side = std::move(links.second);
  p.avatar = kcd2s::local::spawn_player(server.world());
  p.id = server.add_client(p.server_side.get(), p.avatar, 1.0e6f);
  p.peer = std::make_unique<ClientPeer>(schema, p.client_side.get());
  server.set_owner(p.avatar, p.id);
  return p;
}

// Every owner-authored field of NetworkPlayer, the way the game client sends them on each input.
// `marker` lands in motionMask; `animation_fill` selects the animation payload contents.
Input full_input(const SchemaRegistry& schema, std::int64_t marker, char animation_fill) {
  const auto* type = schema.get("NetworkPlayer");
  Input input;
  for (const auto& field : type->fields) {
    if (field.auth != Authority::Owner)
      continue;
    FieldValue value;
    switch (field.id) {
      case kcd2s::local::Position:
        value = Vec3{10.0f, 0.0f, 20.0f};
        break;
      case kcd2s::local::Rotation:
      case kcd2s::local::MotionRotation:
        value = Quat{0.0f, 0.0f, 0.38268343f, 0.92387953f};
        break;
      case kcd2s::local::Grounded:
      case kcd2s::local::PoseValid:
      case kcd2s::local::MotionValid:
      case kcd2s::local::LookValid:
        value = true;
        break;
      case kcd2s::local::LookEnabled:
      case kcd2s::local::UiBlocking:
        value = false;
        break;
      case kcd2s::local::MotionMask:
        value = marker;
        break;
      case kcd2s::local::Animations:
        value = std::string(kAnimationBytes, animation_fill);
        break;
      case kcd2s::local::NativeRiderState:
        value = std::string(kRiderStateBytes, 'r');
        break;
      case kcd2s::local::RagdollPose:
        value = std::string();
        break;
      case kcd2s::local::Outfit:
        value = std::string(kOutfitBytes, 'o');
        break;
      case kcd2s::local::Appearance:
        value = std::string("male||||");
        break;
      case kcd2s::local::BodyVisible:
        value = true;
        break;
      case kcd2s::local::Disguise:
        value = std::string();
        break;
      case kcd2s::local::Seats:
        value = std::string("080000000000021D:0");
        break;
      case kcd2s::local::Hands:
        value = std::string("A0A1A2A3A4A5A6A7A8A9AAABACADAEAF|0102030405060708090A0B0C0D0E0F10");
        break;
      case kcd2s::local::BodyParts:
        value = std::string("henry_body|henry|m_hair_henry|m_beard_00|m_underwear01_m01|male");
        break;
      default:
        if (field.kind == kcd2s::replication::FieldKind::Vec3)
          value = Vec3{1.0f, 2.0f, 3.0f};
        else
          value = std::int64_t{2};
        break;
    }
    input.emplace_back(field.id, std::move(value));
  }
  return input;
}

class Harness {
 public:
  Harness() : schema_(kcd2s::local::player_schema()), server_(schema_) {
    server_.world().set_tick(1);
  }

  Peer& add_peer() {
    peers_.push_back(std::make_unique<Peer>(join(server_, schema_)));
    return *peers_.back();
  }

  // One server tick followed by one tick of every client, like the real loops.
  void pump(int ticks) {
    for (int i = 0; i < ticks; ++i) {
      server_.world().set_tick(server_.world().tick() + 1);
      server_.tick(0.0);
      for (auto& p : peers_)
        p->peer->tick(0.0);
    }
  }

  // Every owner sends `input` once, then all sides tick once.
  void send_and_pump(const Input& input, int ticks) {
    for (int i = 0; i < ticks; ++i) {
      for (auto& p : peers_)
        p->peer->send_input(p->avatar, input);
      pump(1);
    }
  }

  Server& server() {
    return server_;
  }
  const SchemaRegistry& schema() const {
    return schema_;
  }

 private:
  SchemaRegistry schema_;
  Server server_;
  std::vector<std::unique_ptr<Peer>> peers_;
};

bool mirror_matches_world(const ClientPeer& observer, const Server& server, EntityId entity) {
  const auto* mirrored = observer.mirror().get(entity);
  if (!mirrored)
    return false;
  const auto* type = server.world().schema().get("NetworkPlayer");
  for (const auto& field : type->fields) {
    const FieldValue* authoritative = server.world().read(entity, field.id);
    if (!authoritative || mirrored->fields[field.id] != *authoritative)
      return false;
  }
  return true;
}

}  // namespace

KCD2S_TEST(deltas, unchanged_owner_input_sends_empty_snapshots) {
  Harness h;
  Peer& a = h.add_peer();
  Peer& b = h.add_peer();
  h.pump(3);  // SET_OWNER and CONSTRUCTs delivered and acknowledged

  const Input input = full_input(h.schema(), 7, 'a');
  h.send_and_pump(input, 6);  // first input changes every field; let the update be acknowledged
  KCD2S_CHECK(mirror_matches_world(*b.peer, h.server(), a.avatar));
  KCD2S_CHECK(mirror_matches_world(*a.peer, h.server(), b.avatar));

  const Tick animation_stamp =
      h.server().world().field_change_tick(a.avatar, kcd2s::local::Animations);
  const std::uint64_t before = h.server().snapshot_bytes_sent();
  const int steady_ticks = 30;
  h.send_and_pump(input, steady_ticks);
  const std::uint64_t sent = h.server().snapshot_bytes_sent() - before;

  // Two peers, one snapshot each per tick, and nothing changed: only empty snapshots.
  KCD2S_CHECK_EQ(sent, std::uint64_t(steady_ticks) * 2 * kEmptySnapshotBytes);
  KCD2S_CHECK_EQ(h.server().world().field_change_tick(a.avatar, kcd2s::local::Animations),
                 animation_stamp);
}

KCD2S_TEST(deltas, changed_field_still_replicates_alone) {
  Harness h;
  Peer& a = h.add_peer();
  Peer& b = h.add_peer();
  h.pump(3);
  h.send_and_pump(full_input(h.schema(), 1, 'a'), 6);

  // Only A changes, and only its motionMask marker; B keeps sending identical input.
  const Tick animation_stamp =
      h.server().world().field_change_tick(a.avatar, kcd2s::local::Animations);
  a.peer->send_input(a.avatar, full_input(h.schema(), 2, 'a'));
  b.peer->send_input(b.avatar, full_input(h.schema(), 1, 'a'));
  h.pump(1);  // the clients flush their input after this server tick
  const std::uint64_t before = h.server().snapshot_bytes_sent();
  h.pump(1);  // the server applies the input and builds the snapshots
  const std::uint64_t one_tick = h.server().snapshot_bytes_sent() - before;

  const Tick mask_stamp = h.server().world().field_change_tick(a.avatar, kcd2s::local::MotionMask);
  KCD2S_CHECK_EQ(mask_stamp, h.server().world().tick());
  KCD2S_CHECK_EQ(h.server().world().field_change_tick(a.avatar, kcd2s::local::Animations),
                 animation_stamp);
  // The update to B carries one small field, far below the 1220-byte animation state.
  KCD2S_CHECK(one_tick > 2 * kEmptySnapshotBytes);
  KCD2S_CHECK(one_tick < 2 * kEmptySnapshotBytes + 64);

  h.pump(3);
  const auto* seen = b.peer->mirror().get(a.avatar);
  KCD2S_CHECK(seen != nullptr);
  KCD2S_CHECK((std::get<std::int64_t>(seen->fields[kcd2s::local::MotionMask]) == 2));

  // A changed animation payload is still sent and arrives intact.
  h.send_and_pump(full_input(h.schema(), 2, 'z'), 4);
  KCD2S_CHECK(mirror_matches_world(*b.peer, h.server(), a.avatar));
  KCD2S_CHECK(
      (std::get<std::string>(b.peer->mirror().get(a.avatar)->fields[kcd2s::local::Animations]) ==
       std::string(kAnimationBytes, 'z')));
}

KCD2S_TEST(deltas, late_joiner_receives_full_baseline) {
  Harness h;
  Peer& a = h.add_peer();
  h.pump(3);
  const Input input = full_input(h.schema(), 42, 'q');
  h.send_and_pump(input, 6);
  // Many identical inputs: A's field change ticks are now far behind the current tick.
  h.send_and_pump(input, 50);
  KCD2S_CHECK(h.server().world().field_change_tick(a.avatar, kcd2s::local::Animations) + 40 <
              h.server().world().tick());

  Peer& late = h.add_peer();
  h.pump(4);
  // CONSTRUCT carries every field regardless of change ticks, so the late joiner has A's full,
  // current state, including the old animation payload.
  KCD2S_CHECK(mirror_matches_world(*late.peer, h.server(), a.avatar));
  const auto* seen = late.peer->mirror().get(a.avatar);
  KCD2S_CHECK(seen != nullptr);
  KCD2S_CHECK((std::get<std::string>(seen->fields[kcd2s::local::Animations]) ==
               std::string(kAnimationBytes, 'q')));
  KCD2S_CHECK((std::get<std::int64_t>(seen->fields[kcd2s::local::MotionMask]) == 42));
  // And the existing peer sees the late joiner.
  KCD2S_CHECK(mirror_matches_world(*a.peer, h.server(), late.avatar));
}

KCD2S_TEST(deltas, epoch_fence_still_rejects_stale_identical_input) {
  Harness h;
  Peer& a = h.add_peer();
  Peer& b = h.add_peer();
  h.pump(3);
  h.send_and_pump(full_input(h.schema(), 5, 'a'), 6);

  // Force state: the server bumps A's epoch. Input queued under the old epoch is dropped even if
  // its values differ, and once A adopts the new epoch its identical input stays delta-free.
  const Tick mask_stamp = h.server().world().field_change_tick(a.avatar, kcd2s::local::MotionMask);
  h.server().force_state(a.avatar);
  // Encoded now, under the old epoch; it reaches the server after the fence.
  a.peer->send_input(a.avatar, full_input(h.schema(), 99, 'a'));
  h.pump(4);  // the stale input is dropped and A adopts the FORCE_STATE epoch
  KCD2S_CHECK(
      (std::get<std::int64_t>(*h.server().world().read(a.avatar, kcd2s::local::MotionMask)) == 5));
  KCD2S_CHECK_EQ(h.server().world().field_change_tick(a.avatar, kcd2s::local::MotionMask),
                 mask_stamp);
  // No field changed, yet the observer's replica carries the new epoch (empty-mask UPDATE).
  const std::uint8_t epoch = h.server().world().slot(a.avatar)->stateEpoch;
  KCD2S_CHECK_EQ(epoch, std::uint8_t{1});
  KCD2S_CHECK_EQ(b.peer->mirror().get(a.avatar)->epoch, epoch);
  h.send_and_pump(full_input(h.schema(), 6, 'a'), 4);
  KCD2S_CHECK(
      (std::get<std::int64_t>(*h.server().world().read(a.avatar, kcd2s::local::MotionMask)) == 6));
  KCD2S_CHECK(mirror_matches_world(*b.peer, h.server(), a.avatar));
}

KCD2S_TEST(deltas, owner_handoff_epoch_reaches_observers_without_field_change) {
  Harness h;
  Peer& a = h.add_peer();
  Peer& b = h.add_peer();
  Peer& c = h.add_peer();
  // An extra entity owned by C (not C's viewer), so it survives C's departure and is re-elected.
  const EntityId prop = kcd2s::local::spawn_player(h.server().world());
  h.server().set_owner(prop, c.id);
  h.pump(3);
  h.send_and_pump(full_input(h.schema(), 3, 'a'), 6);
  KCD2S_CHECK(b.peer->mirror().get(prop) != nullptr);
  KCD2S_CHECK_EQ(b.peer->mirror().get(prop)->epoch, std::uint8_t{0});

  h.server().remove_client(c.id);
  h.pump(4);
  const auto* slot = h.server().world().slot(prop);
  KCD2S_CHECK(slot != nullptr);
  KCD2S_CHECK(slot->owner == a.id || slot->owner == b.id);
  KCD2S_CHECK_EQ(slot->stateEpoch, std::uint8_t{1});
  KCD2S_CHECK_EQ(a.peer->mirror().get(prop)->epoch, std::uint8_t{1});
  KCD2S_CHECK_EQ(b.peer->mirror().get(prop)->epoch, std::uint8_t{1});
}
