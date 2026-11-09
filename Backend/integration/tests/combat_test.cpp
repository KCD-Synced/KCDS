#include "../local/native_event_relay.hpp"
#include "kcd2s/integration/server.hpp"
#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/testkit.hpp"
#include <limits>
#include <stdexcept>
#include "kcd2s/core/bits.hpp"
#include "kcd2s/wire/packet.hpp"
#include <algorithm>
#include <functional>
using namespace kcd2s;
using namespace kcd2s::integration;
namespace {
NativeMeleeData cause() {
  return NativeMeleeData{};
}
struct MutatingLink : transport::IDatagramLink {
  transport::IDatagramLink* link;
  std::function<void(MeleeEvent&)> change;
  bool drop_snapshots = false;
  explicit MutatingLink(transport::IDatagramLink* value) : link(value) {
  }
  void send(const std::uint8_t* bytes, std::size_t size) override {
    wire::Packet packet;
    if (!change || !wire::read_packet(bytes, size, packet)) {
      link->send(bytes, size);
      return;
    }
    for (auto& channel : packet.channels)
      if (channel.channel == kCombatChannel) {
        core::BitReader reader(channel.bytes);
        core::BitWriter writer;
        auto count = reader.read_bits(16);
        writer.write_bits(count, 16);
        for (std::uint64_t n = 0; n < count; ++n) {
          auto seq = reader.read_bits(16), length = reader.read_bits(16);
          std::vector<std::uint8_t> body;
          for (std::uint64_t i = 0; i < length; ++i)
            body.push_back(std::uint8_t(reader.read_bits(8)));
          MeleeEvent e;
          if (decode_melee(body, e)) {
            change(e);
            body = encode_melee(e);
          }
          writer.write_bits(seq, 16);
          writer.write_bits(body.size(), 16);
          for (auto b : body)
            writer.write_bits(b, 8);
        }
        channel.bytes = writer.bytes();
      }
    link->send(wire::write_packet(packet));
  }
  bool recv(transport::Datagram& bytes) override {
    if (!link->recv(bytes)) {
      return false;
    }
    wire::Packet packet;
    if (drop_snapshots && wire::read_packet(bytes.data(), bytes.size(), packet)) {
      std::erase_if(packet.channels, [](const wire::ChannelPayload& channel) {
        const bool snapshot = channel.channel == static_cast<std::uint8_t>(wire::Channel::Snapshot);
        return snapshot;
      });
      bytes = wire::write_packet(packet);
    }
    return true;
  }
  void poll() override {
    link->poll();
  }
};
struct Pair {
  replication::SchemaRegistry schema;
  std::unique_ptr<Server> server;
  std::pair<std::unique_ptr<transport::SimLink>, std::unique_ptr<transport::SimLink>> al, bl;
  std::unique_ptr<ClientPeer> a, b;
  std::pair<std::unique_ptr<transport::SimLink>, std::unique_ptr<transport::SimLink>> cl;
  std::unique_ptr<ClientPeer> c;
  core::EntityId ce{};
  core::PeerId cp{};
  std::unique_ptr<MutatingLink> mutate;
  core::EntityId ae{}, be{};
  core::PeerId ap{}, bp{};
  double now = 0;
  explicit Pair(bool lossy = false) {
    auto type = schema.define("Player")
                    .field("position", replication::FieldKind::Vec3, replication::Priority::High,
                           replication::Authority::Owner)
                    .id();
    server = std::make_unique<Server>(schema);
    ae = server->world().spawn(type);
    be = server->world().spawn(type);
    server->world().write(ae, 0, core::Vec3{});
    server->world().write(be, 0, core::Vec3{});
    transport::LinkParams p;
    if (lossy) {
      p.loss = .25;
      p.duplicate = .4;
      p.min_delay = 1;
      p.max_delay = 5;
      p.seed = 777;
    }
    al = transport::make_sim_pair(p, p);
    p.seed = 123;
    bl = transport::make_sim_pair(p, p);
    ap = server->add_client(al.first.get(), ae, 10000);
    bp = server->add_client(bl.first.get(), be, 10000);
    server->configure_world(ap, {});
    server->configure_world(bp, {});
    server->set_owner(ae, ap);
    server->set_owner(be, bp);
    mutate = std::make_unique<MutatingLink>(al.second.get());
    a = std::make_unique<ClientPeer>(schema, mutate.get());
    b = std::make_unique<ClientPeer>(schema, bl.second.get());
    pump(240);
    if (!a->mark_world_ready(1) || !b->mark_world_ready(1))
      throw std::runtime_error("fixture world not ready");
    pump(240);
  }
  void add_observer() {
    ce = server->world().spawn(server->world().slot(ae)->type);
    server->world().write(ce, 0, core::Vec3{});
    cl = transport::make_sim_pair({}, {});
    cp = server->add_client(cl.first.get(), ce, 10000);
    server->configure_world(cp, {});
    server->set_owner(ce, cp);
    c = std::make_unique<ClientPeer>(schema, cl.second.get());
    pump(240);
    if (!c->mark_world_ready(1)) {
      throw std::runtime_error("observer world not ready");
    }
    pump(240);
  }

  void pump(int n) {
    for (int i = 0; i < n; ++i) {
      now += 1.0 / 60;
      server->world().set_tick(server->world().tick() + 1);
      al.first->poll();
      al.second->poll();
      bl.first->poll();
      bl.second->poll();
      if (c) {
        cl.first->poll();
        cl.second->poll();
      }
      server->tick(now);
      a->tick(now);
      b->tick(now);
      if (c) {
        c->tick(now);
      }
    }
  }

  bool send_contact_or_effects(bool effects) {
    if (!effects) {
      return a->send_native_contact(ae, be, NativeContactData{});
    }
    NativeEffectRecord health;
    health.kind = 1;
    health.soul_role = 2;
    const float delta = -5.0f;
    std::memcpy(health.value.data() + 4, &delta, sizeof(delta));
    return b->send_native_effects(ae, be, cause(), {health});
  }

  ClientPeer& combat_recipient(bool effects) {
    return effects ? *a : *b;
  }
};
}  // namespace
KCD2S_TEST(combat, original_cause_reliable_exactly_once_under_loss_and_reorder) {
  Pair p(true);
  auto data = cause();
  for (int i = 0; i < 12; ++i) {
    data[84] = std::uint8_t(i);
    KCD2S_CHECK(p.a->send_melee(p.ae, p.be, data));
  }
  p.pump(1200);
  auto received = p.b->take_melee();
  KCD2S_CHECK_EQ(received.size(), std::size_t(12));
  for (std::size_t i = 0; i < received.size(); ++i) {
    KCD2S_CHECK_EQ(received[i].sequence, i + 1);
    KCD2S_CHECK_EQ(received[i].data[84], std::uint8_t(i));
  }
  KCD2S_CHECK(p.a->take_melee().empty());
  p.pump(120);
  KCD2S_CHECK(p.b->take_melee().empty());
}

KCD2S_TEST(combat, attack_generation_codec_rejects_malformed_or_mixed_messages) {
  MeleeEvent event;
  event.revision = 1;
  event.source = 1;
  event.target = 2;
  event.sequence = 1;
  event.attack_generation_marker = true;
  event.attack_generation = 123;
  event.native_attack_kind = 3;
  auto bytes = encode_melee(event);
  KCD2S_CHECK_EQ(bytes.size(), std::size_t(36));
  MeleeEvent decoded;
  KCD2S_CHECK(decode_melee(bytes, decoded));
  KCD2S_CHECK(decoded.attack_generation_marker);
  KCD2S_CHECK_EQ(decoded.attack_generation, std::uint64_t(123));
  bytes.back() = 7;
  KCD2S_CHECK(!decode_melee(bytes, decoded));
  event.contact = true;
  KCD2S_CHECK(encode_melee(event).empty());
  event.attack_generation_marker = false;
  event.native_attack_kind = 0;
  bytes = encode_melee(event);
  KCD2S_CHECK_EQ(bytes.size(), std::size_t(505));
  KCD2S_CHECK(decode_melee(bytes, decoded));
  KCD2S_CHECK(decoded.contact);
  KCD2S_CHECK_EQ(decoded.attack_generation, event.attack_generation);
  std::fill_n(bytes.begin() + 27, 8, std::uint8_t(0));
  KCD2S_CHECK(!decode_melee(bytes, decoded));
  event.attack_generation = 0;
  bytes = encode_melee(event);
  KCD2S_CHECK_EQ(bytes.size(), std::size_t(497));
  KCD2S_CHECK(decode_melee(bytes, decoded));
  event.contact = false;
  event.attack_generation = 123;
  KCD2S_CHECK(encode_melee(event).empty());
}

KCD2S_TEST(combat, observer_contact_is_scoped_and_keeps_owner_delivery) {
  Pair p;
  p.add_observer();
  for (const auto entity : {p.ae, p.be, p.ce}) {
    p.server->world().slot(entity)->dimension = 1;
  }
  p.pump(30);
  KCD2S_CHECK(p.a->send_attack_generation(p.ae, p.ce, 10, 3));
  KCD2S_CHECK(p.a->send_combat_action(p.ae, p.ce, {CombatAction::AttackMouse, 1, 1}));
  p.pump(60);
  auto events = p.c->take_melee();
  KCD2S_CHECK_EQ(events.size(), std::size_t(2));
  KCD2S_CHECK(events[0].attack_generation_marker);
  KCD2S_CHECK(events[1].input);
  KCD2S_CHECK(events[0].sequence < events[1].sequence);
  KCD2S_CHECK(p.a->send_native_contact(p.ae, p.be, {}, 10));
  p.pump(60);
  events = p.c->take_melee();
  KCD2S_CHECK_EQ(events.size(), std::size_t(1));
  KCD2S_CHECK(p.c->current_combat_context(events[0]));
  KCD2S_CHECK_EQ(p.b->take_melee().size(), std::size_t(1));
  // A spectator moving to another world must lose preview access immediately,
  // even before acknowledging its pending DESTRUCTs. The duel stays in scope.
  p.server->world().slot(p.ce)->dimension = 99;
  KCD2S_CHECK(p.a->send_native_contact(p.ae, p.be, {}, 11));
  p.pump(60);
  KCD2S_CHECK(p.c->take_melee().empty());
  KCD2S_CHECK_EQ(p.b->take_melee().size(), std::size_t(1));
  KCD2S_CHECK(!p.c->current_combat_context(events[0]));
}

// Lag compensation: a contact between players who were never within reach in the last half
// second is counted, and dropped when the check enforces.
KCD2S_TEST(combat, contacts_out_of_reach_are_counted_and_dropped_when_enforced) {
  Pair p;
  p.server->set_melee_reach_check(MeleeReachMode::Enforce);
  p.server->world().write(p.be, 0, core::Vec3{20, 0, 0});
  p.pump(40);
  p.b->take_melee();
  KCD2S_CHECK(p.a->send_native_contact(p.ae, p.be, {}, 10));
  p.pump(10);
  KCD2S_CHECK(p.b->take_melee().empty());
  KCD2S_CHECK_EQ(p.server->melee_refusals(), std::uint64_t(1));
  // Observed only: counted, still delivered.
  p.server->set_melee_reach_check(MeleeReachMode::Observe);
  KCD2S_CHECK(p.a->send_native_contact(p.ae, p.be, {}, 11));
  p.pump(10);
  KCD2S_CHECK_EQ(p.b->take_melee().size(), std::size_t(1));
  KCD2S_CHECK_EQ(p.server->melee_refusals(), std::uint64_t(2));
  // Within reach: delivered, nothing counted.
  p.server->set_melee_reach_check(MeleeReachMode::Enforce);
  p.server->world().write(p.be, 0, core::Vec3{2, 0, 0});
  p.pump(5);
  KCD2S_CHECK(p.a->send_native_contact(p.ae, p.be, {}, 12));
  p.pump(10);
  KCD2S_CHECK_EQ(p.b->take_melee().size(), std::size_t(1));
  KCD2S_CHECK_EQ(p.server->melee_refusals(), std::uint64_t(2));
}

// A client whose game build lacks combat (CLIENT_NATIVES) neither sends nor takes a blow.
KCD2S_TEST(combat, a_client_without_combat_gets_and_sends_no_blow) {
  Pair p;
  ClientNatives natives;
  natives.found = 700;
  natives.combat = false;
  KCD2S_CHECK(p.b->send_client_natives(natives));
  p.pump(5);
  KCD2S_CHECK(p.a->send_native_contact(p.ae, p.be, {}, 10));
  p.pump(10);
  KCD2S_CHECK(p.b->take_melee().empty());
  KCD2S_CHECK(p.b->send_native_contact(p.be, p.ae, {}, 11));
  p.pump(10);
  KCD2S_CHECK(p.a->take_melee().empty());
}

KCD2S_TEST(combat, observer_preview_budget_preserves_all_victim_contacts) {
  Pair p;
  p.add_observer();
  for (unsigned i = 0; i < 20; ++i) {
    KCD2S_CHECK(p.a->send_native_contact(p.ae, p.be, {}, i + 1));
  }
  p.pump(60);
  KCD2S_CHECK_EQ(p.b->take_melee().size(), std::size_t(20));
  const auto previews = p.c->take_melee();
  KCD2S_CHECK(!previews.empty());
  KCD2S_CHECK(previews.size() <= kMaxObserverContactsPerTick);
  // A non-draining observer has its own bounded queue. It cannot displace
  // the contacts which the local victim must commit.
  for (unsigned batch = 0; batch < 20; ++batch) {
    for (unsigned i = 0; i < kMaxObserverContactsPerTick; ++i) {
      KCD2S_CHECK(p.a->send_native_contact(p.ae, p.be, {}, 100 + batch * 4 + i));
    }
    p.pump(8);
  }
  KCD2S_CHECK_EQ(p.b->take_melee().size(), std::size_t(80));
  KCD2S_CHECK_EQ(p.c->take_melee().size(), kMaxObserverContacts);
}

KCD2S_TEST(combat, victim_reaction_reaches_the_attacker_and_every_watcher) {
  Pair p;
  p.add_observer();
  NativeReactionData reaction{};
  reaction[68] = 4;
  // The attacker's contact, then the victim's own answer to it and its hit-action marker.
  KCD2S_CHECK(p.a->send_native_contact(p.ae, p.be, {}, 10));
  p.pump(60);
  KCD2S_CHECK_EQ(p.b->take_melee().size(), std::size_t(1));
  KCD2S_CHECK_EQ(p.c->take_melee().size(), std::size_t(1));
  KCD2S_CHECK(p.b->send_reaction(p.ae, p.be, reaction, true));
  KCD2S_CHECK(p.b->send_combat_action(p.be, p.ae, {CombatAction::HitAction, 1, 0}));
  KCD2S_CHECK(p.b->send_combat_action(p.be, p.ce, {CombatAction::HitAction, 1, 0}));
  p.pump(60);
  const auto attacker = p.a->take_melee();
  const auto watcher = p.c->take_melee();
  KCD2S_CHECK_EQ(attacker.size(), std::size_t(2));
  KCD2S_CHECK_EQ(watcher.size(), std::size_t(2));
  // The watcher takes its own inputs before its copies of other players' blows; the client pairs
  // the marker with the reaction in either order.
  for (const auto* events : {&attacker, &watcher}) {
    const auto reaction_at =
        std::find_if(events->begin(), events->end(), [](const auto& e) { return e.reaction; });
    const auto marker_at = std::find_if(events->begin(), events->end(), [](const auto& e) {
      return e.input && e.input_data.action == CombatAction::HitAction;
    });
    KCD2S_CHECK(reaction_at != events->end());
    KCD2S_CHECK(marker_at != events->end());
    if (reaction_at != events->end()) {
      KCD2S_CHECK(reaction_at->authority_is_target);
      KCD2S_CHECK_EQ(reaction_at->reaction_data[68], std::uint8_t(4));
    }
  }
  // The attacker gets both in the victim's order.
  if (attacker.size() == 2) {
    KCD2S_CHECK(attacker[0].reaction);
    KCD2S_CHECK(attacker[1].input);
  }
}
KCD2S_TEST(combat, legacy_and_synchronized_contacts_are_never_observer_previews) {
  Pair p;
  p.add_observer();
  KCD2S_CHECK(!p.c->send_attack_generation(p.ae, p.be, 10, 3));
  KCD2S_CHECK(!p.a->send_attack_generation(p.ae, p.ce, 10, 7));
  KCD2S_CHECK(p.a->send_native_contact(p.ae, p.be, {}));
  NativeContactData paired{};
  paired[304] = 1;
  KCD2S_CHECK(p.a->send_native_contact(p.ae, p.be, paired, 11));
  p.pump(60);
  KCD2S_CHECK(p.c->take_melee().empty());
  KCD2S_CHECK_EQ(p.b->take_melee().size(), std::size_t(2));
}
KCD2S_TEST(combat, only_owner_can_emit_and_only_target_receives) {
  Pair p;
  KCD2S_CHECK(!p.a->send_melee(p.be, p.ae, cause()));
  KCD2S_CHECK(!p.a->send_melee(p.ae, p.ae, cause()));
  KCD2S_CHECK(p.a->send_melee(p.ae, p.be, cause()));
  p.pump(10);
  KCD2S_CHECK_EQ(p.b->take_melee().size(), std::size_t(1));
  KCD2S_CHECK(p.a->take_melee().empty());
}
KCD2S_TEST(combat, source_and_target_forced_epoch_fence_in_flight_hit) {
  for (bool source : {false, true}) {
    Pair p;
    KCD2S_CHECK(p.a->send_melee(p.ae, p.be, cause()));
    p.a->tick(p.now);
    p.server->force_state(source ? p.ae : p.be);
    p.pump(12);
    KCD2S_CHECK(p.b->take_melee().empty());
    KCD2S_CHECK(p.a->send_melee(p.ae, p.be, cause()));
    p.pump(12);
    KCD2S_CHECK_EQ(p.b->take_melee().size(), std::size_t(1));
  }
}

KCD2S_TEST(combat, current_epoch_requires_a_known_live_entity) {
  Pair p;
  KCD2S_CHECK(!p.a->combat_epoch(core::kInvalidEntity));
  KCD2S_CHECK(!p.a->combat_epoch(std::numeric_limits<core::EntityId>::max()));
  const auto owner_epoch = p.a->combat_epoch(p.ae);
  const auto remote_epoch = p.a->combat_epoch(p.be);
  KCD2S_CHECK(owner_epoch.has_value());
  KCD2S_CHECK(remote_epoch.has_value());
  if (owner_epoch && remote_epoch) {
    KCD2S_CHECK_EQ(*owner_epoch, p.server->world().slot(p.ae)->stateEpoch);
    KCD2S_CHECK_EQ(*remote_epoch, p.a->mirror().get(p.be)->epoch);
  }
}

KCD2S_TEST(combat, current_epoch_adopts_forced_owner_and_remote_fences) {
  Pair p;
  // Keep A's mirror on the old snapshot while ordered force controls still arrive. Returning
  // only the mirror epoch would authorize an earlier-life contact during this cross-channel gap.
  p.mutate->drop_snapshots = true;
  for (const auto entity : {p.ae, p.be}) {
    const auto before = p.a->combat_epoch(entity);
    KCD2S_CHECK(before.has_value());
    p.server->force_state(entity);
    p.pump(12);
    const auto owner = p.a->combat_epoch(entity);
    const auto remote = p.b->combat_epoch(entity);
    KCD2S_CHECK(owner.has_value());
    KCD2S_CHECK(remote.has_value());
    if (before && owner && remote) {
      KCD2S_CHECK_EQ(*owner, std::uint8_t(*before + 1));
      KCD2S_CHECK_EQ(*owner, *remote);
      KCD2S_CHECK_EQ(p.a->mirror().get(entity)->epoch, *before);
    }
  }
}

KCD2S_TEST(combat, current_epoch_drops_adopted_entries_after_entity_despawn) {
  for (const bool source : {false, true}) {
    Pair p;
    const auto entity = source ? p.ae : p.be;
    p.server->force_state(entity);
    p.pump(12);
    KCD2S_CHECK(p.a->combat_epoch(entity).has_value());
    p.server->despawn(entity);
    p.pump(12);
    KCD2S_CHECK(!p.a->mirror().get(entity));
    KCD2S_CHECK(!p.a->combat_epoch(entity));
    KCD2S_CHECK(!p.b->combat_epoch(entity));
  }
}

KCD2S_TEST(combat, current_epoch_is_unavailable_during_world_reset) {
  Pair p;
  KCD2S_CHECK(p.a->combat_epoch(p.ae).has_value());
  WorldConfig next;
  next.revision = 2;
  KCD2S_CHECK(p.server->configure_world(p.ap, next));
  KCD2S_CHECK(p.server->configure_world(p.bp, next));
  p.pump(12);
  KCD2S_CHECK(!p.a->combat_epoch(p.ae));
  KCD2S_CHECK(!p.a->combat_epoch(p.be));
  KCD2S_CHECK(p.a->mark_world_ready(next.revision));
  KCD2S_CHECK(p.b->mark_world_ready(next.revision));
  p.pump(12);
  const auto epoch = p.a->combat_epoch(p.ae);
  KCD2S_CHECK(epoch.has_value());
  if (epoch) {
    KCD2S_CHECK_EQ(*epoch, p.a->mirror().get(p.ae)->epoch);
  }
}

KCD2S_TEST(combat, current_context_preserves_queued_and_held_contacts_and_effects) {
  for (const bool effects : {false, true}) {
    Pair p;
    KCD2S_CHECK(p.send_contact_or_effects(effects));
    p.pump(12);
    auto& recipient = p.combat_recipient(effects);
    const auto received = recipient.take_melee();
    KCD2S_CHECK_EQ(received.size(), std::size_t(1));
    if (received.empty()) {
      continue;
    }
    KCD2S_CHECK(recipient.current_combat_context(received.front()));
    KCD2S_CHECK(received.front().effects == effects);
    KCD2S_CHECK(received.front().contact != effects);
    KCD2S_CHECK(recipient.take_melee().empty());
  }
}

KCD2S_TEST(combat, queued_and_held_combat_reject_either_epoch_changed_before_drain) {
  for (const bool effects : {false, true}) {
    for (const bool source : {false, true}) {
      Pair p;
      KCD2S_CHECK(p.send_contact_or_effects(effects));
      p.pump(12);
      auto& recipient = p.combat_recipient(effects);
      const auto held = recipient.take_melee();
      KCD2S_CHECK_EQ(held.size(), std::size_t(1));
      if (held.empty()) {
        continue;
      }
      KCD2S_CHECK(recipient.current_combat_context(held.front()));
      KCD2S_CHECK(p.send_contact_or_effects(effects));
      p.pump(12);
      p.server->force_state(source ? p.ae : p.be);
      p.pump(12);
      KCD2S_CHECK(!recipient.current_combat_context(held.front()));
      KCD2S_CHECK(recipient.take_melee().empty());
    }
  }
}

KCD2S_TEST(combat, queued_and_held_combat_reject_either_entity_despawned_before_drain) {
  for (const bool effects : {false, true}) {
    for (const bool source : {false, true}) {
      Pair p;
      KCD2S_CHECK(p.send_contact_or_effects(effects));
      p.pump(12);
      auto& recipient = p.combat_recipient(effects);
      const auto held = recipient.take_melee();
      KCD2S_CHECK_EQ(held.size(), std::size_t(1));
      if (held.empty()) {
        continue;
      }
      KCD2S_CHECK(p.send_contact_or_effects(effects));
      p.pump(12);
      const auto removed = source ? p.ae : p.be;
      p.server->despawn(removed);
      p.pump(12);
      KCD2S_CHECK(!recipient.mirror().get(removed));
      KCD2S_CHECK(!recipient.current_combat_context(held.front()));
      KCD2S_CHECK(recipient.take_melee().empty());
    }
  }
}

KCD2S_TEST(combat, held_combat_requires_the_recipient_ownership_grant) {
  for (const bool effects : {false, true}) {
    Pair p;
    KCD2S_CHECK(p.send_contact_or_effects(effects));
    p.pump(12);
    auto& recipient = p.combat_recipient(effects);
    const auto held = recipient.take_melee();
    KCD2S_CHECK_EQ(held.size(), std::size_t(1));
    if (held.empty()) {
      continue;
    }
    auto wrong_recipient = held.front();
    wrong_recipient.authority_is_target = !wrong_recipient.authority_is_target;
    KCD2S_CHECK(!recipient.current_combat_context(wrong_recipient));
    KCD2S_CHECK(!p.combat_recipient(!effects).current_combat_context(held.front()));
  }
}

KCD2S_TEST(combat, revive_rejects_prequeued_and_held_combat_without_changing_equipment) {
  for (const bool effects : {false, true}) {
    for (const bool source : {false, true}) {
      Pair p;
      KCD2S_CHECK(p.send_contact_or_effects(effects));
      p.pump(12);
      auto& recipient = p.combat_recipient(effects);
      const auto held = recipient.take_melee();
      KCD2S_CHECK_EQ(held.size(), std::size_t(1));
      if (held.empty()) {
        continue;
      }
      KCD2S_CHECK(p.send_contact_or_effects(effects));
      p.pump(12);
      auto& knocked_out = source ? *p.a : *p.b;
      KCD2S_CHECK(knocked_out.send_knocked_out());
      p.pump(12);
      const auto revived = source ? p.ae : p.be;
      const auto epoch_before = p.server->world().slot(revived)->stateEpoch;
      KCD2S_CHECK(p.server->revive(revived));
      KCD2S_CHECK(p.server->world().slot(revived)->stateEpoch != epoch_before);
      p.pump(12);
      KCD2S_CHECK(!recipient.current_combat_context(held.front()));
      KCD2S_CHECK(recipient.take_melee().empty());
      KCD2S_CHECK_EQ(knocked_out.take_revives(), std::size_t(1));
      KCD2S_CHECK(p.send_contact_or_effects(effects));
      p.pump(12);
      const auto fresh = recipient.take_melee();
      KCD2S_CHECK_EQ(fresh.size(), std::size_t(1));
      if (!fresh.empty()) {
        KCD2S_CHECK(recipient.current_combat_context(fresh.front()));
      }
    }
  }
}

KCD2S_TEST(combat, revive_rejects_inflight_old_epoch_and_accepts_fresh_combat) {
  for (const bool effects : {false, true}) {
    for (const bool source : {false, true}) {
      Pair p;
      auto& knocked_out = source ? *p.a : *p.b;
      KCD2S_CHECK(knocked_out.send_knocked_out());
      p.pump(12);
      KCD2S_CHECK(p.send_contact_or_effects(effects));
      auto& sender = effects ? *p.b : *p.a;
      sender.tick(p.now);
      KCD2S_CHECK(p.server->revive(source ? p.ae : p.be));
      p.pump(12);
      auto& recipient = p.combat_recipient(effects);
      KCD2S_CHECK(recipient.take_melee().empty());
      KCD2S_CHECK(p.send_contact_or_effects(effects));
      p.pump(12);
      const auto fresh = recipient.take_melee();
      KCD2S_CHECK_EQ(fresh.size(), std::size_t(1));
      if (!fresh.empty()) {
        KCD2S_CHECK(recipient.current_combat_context(fresh.front()));
      }
    }
  }
}

KCD2S_TEST(combat, queued_and_held_combat_reject_world_reset_before_drain) {
  for (const bool effects : {false, true}) {
    Pair p;
    KCD2S_CHECK(p.send_contact_or_effects(effects));
    p.pump(12);
    auto& recipient = p.combat_recipient(effects);
    const auto held = recipient.take_melee();
    KCD2S_CHECK_EQ(held.size(), std::size_t(1));
    if (held.empty()) {
      continue;
    }
    KCD2S_CHECK(p.send_contact_or_effects(effects));
    p.pump(12);
    WorldConfig next;
    next.revision = 2;
    KCD2S_CHECK(p.server->configure_world(p.ap, next));
    KCD2S_CHECK(p.server->configure_world(p.bp, next));
    p.pump(12);
    KCD2S_CHECK(!recipient.world_ready());
    auto current_revision = held.front();
    current_revision.revision = next.revision;
    KCD2S_CHECK(!recipient.current_combat_context(current_revision));
    KCD2S_CHECK(!recipient.current_combat_context(held.front()));
    KCD2S_CHECK(recipient.take_melee().empty());
    KCD2S_CHECK(p.a->mark_world_ready(next.revision));
    KCD2S_CHECK(p.b->mark_world_ready(next.revision));
    p.pump(12);
    KCD2S_CHECK(!recipient.current_combat_context(held.front()));
    KCD2S_CHECK(p.send_contact_or_effects(effects));
    p.pump(12);
    const auto after_reset = recipient.take_melee();
    KCD2S_CHECK_EQ(after_reset.size(), std::size_t(1));
    if (!after_reset.empty()) {
      KCD2S_CHECK(recipient.current_combat_context(after_reset.front()));
    }
  }
}

KCD2S_TEST(combat, changing_world_rejects_old_cause_and_waits_for_ready) {
  Pair p;
  KCD2S_CHECK(p.a->send_melee(p.ae, p.be, cause()));
  p.a->tick(p.now);
  WorldConfig c;
  c.revision = 2;
  c.map = "klaster";
  KCD2S_CHECK(p.server->configure_world(p.ap, c));
  KCD2S_CHECK(p.server->configure_world(p.bp, c));
  p.pump(12);
  KCD2S_CHECK(p.b->take_melee().empty());
  KCD2S_CHECK(!p.a->send_melee(p.ae, p.be, cause()));
  KCD2S_CHECK(p.a->mark_world_ready(2));
  KCD2S_CHECK(p.b->mark_world_ready(2));
  p.pump(8);
  KCD2S_CHECK(p.a->send_melee(p.ae, p.be, cause()));
  p.pump(8);
  KCD2S_CHECK_EQ(p.b->take_melee().size(), std::size_t(1));
}
KCD2S_TEST(combat, malformed_or_process_local_ids_are_rejected) {
  MeleeEvent e;
  e.revision = 1;
  e.source = 1;
  e.target = 2;
  e.sequence = 1;
  e.data = cause();
  auto good = encode_melee(e);
  MeleeEvent out;
  KCD2S_CHECK(decode_melee(good, out));
  for (std::size_t n = 0; n < good.size(); ++n)
    KCD2S_CHECK(!decode_melee({good.begin(), good.begin() + n}, out));
  auto trailing = good;
  trailing.push_back(0);
  KCD2S_CHECK(!decode_melee(trailing, out));
  for (unsigned at : {0u, 8u, 16u, 24u, 120u, 128u, 136u}) {
    auto bad = e;
    melee_set_u64(bad.data, at, 0x100005980);
    KCD2S_CHECK(encode_melee(bad).empty());
  }
  auto bad = e;
  float nan = std::numeric_limits<float>::quiet_NaN();
  std::memcpy(bad.data.data() + 44, &nan, 4);
  KCD2S_CHECK(encode_melee(bad).empty());
}

KCD2S_TEST(combat, captured_native_unarmed_cause_preserves_original_values) {
  // Actual native hit recorded after manual player input; no invented damage values.
  const NativeMeleeData native = {
      0x51, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x77, 0x77, 0x00, 0x00, 0x67, 0x02, 0x00,
      0x00, 0x98, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x06, 0x00, 0x04, 0x00, 0x67, 0x02,
      0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x80, 0x3f, 0x00, 0x00, 0x80, 0x3f, 0x88, 0xd7, 0x16, 0xbc, 0xc9, 0x55, 0x8a, 0xbd,
      0x00, 0x00, 0x00, 0x00, 0x8f, 0xe3, 0xef, 0x42, 0x6b, 0x16, 0xff, 0x44, 0x4b, 0x14, 0x66,
      0x42, 0x14, 0x00, 0x00, 0x00, 0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff,
      0xff, 0xff, 0xff, 0x00, 0x00, 0x80, 0x3f, 0xce, 0x56, 0x1c, 0x3d, 0x2b, 0x02, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  KCD2S_CHECK(!valid_melee_data(native));
  auto portable = portable_melee_data(native);
  KCD2S_CHECK(valid_melee_data(portable));
  for (unsigned i = 32; i < 144; ++i)
    if (i != 102 && i != 103)
      KCD2S_CHECK_EQ(portable[i], native[i]);
  MeleeEvent e;
  e.source = 1;
  e.target = 2;
  e.sequence = 1;
  e.revision = 1;
  e.data = portable;
  MeleeEvent decoded;
  KCD2S_CHECK(decode_melee(encode_melee(e), decoded));
  KCD2S_CHECK(decoded.data == portable);
}

KCD2S_TEST(combat, server_rejects_forged_owner_on_wire) {
  Pair p;
  p.mutate->change = [&](MeleeEvent& e) {
    e.source = p.be;
    e.target = p.ae;
  };
  KCD2S_CHECK(p.a->send_melee(p.ae, p.be, cause()));
  p.pump(20);
  KCD2S_CHECK(p.a->take_melee().empty());
  KCD2S_CHECK(p.b->take_melee().empty());
  p.mutate->change = {};
  KCD2S_CHECK(p.a->send_melee(p.ae, p.be, cause()));
  p.pump(20);
  KCD2S_CHECK_EQ(p.b->take_melee().size(), std::size_t(1));
}
KCD2S_TEST(combat, server_deduplicates_native_transaction_repeated_in_new_messages) {
  Pair p;
  p.mutate->change = [](MeleeEvent& e) { e.sequence = 1; };
  KCD2S_CHECK(p.a->send_melee(p.ae, p.be, cause()));
  KCD2S_CHECK(p.a->send_melee(p.ae, p.be, cause()));
  p.pump(20);
  KCD2S_CHECK_EQ(p.b->take_melee().size(), std::size_t(1));
}

KCD2S_TEST(combat, damage_then_reaction_remain_ordered_under_loss) {
  Pair p(true);
  NativeReactionData reaction{};
  reaction[68] = 4;
  reaction[172] = 1;
  for (int n = 0; n < 8; ++n) {
    KCD2S_CHECK(p.a->send_melee(p.ae, p.be, cause()));
    reaction[52] = std::uint8_t(n);
    KCD2S_CHECK(p.a->send_reaction(p.ae, p.be, reaction));
  }
  p.pump(1200);
  auto events = p.b->take_melee();
  KCD2S_CHECK_EQ(events.size(), std::size_t(16));
  for (std::size_t i = 0; i < events.size(); ++i) {
    KCD2S_CHECK_EQ(events[i].sequence, i + 1);
    KCD2S_CHECK_EQ(events[i].reaction, (i % 2) != 0);
    if (i % 2)
      KCD2S_CHECK_EQ(events[i].reaction_data[52], std::uint8_t(i / 2));
  }
  KCD2S_CHECK(p.a->take_melee().empty());
}
KCD2S_TEST(combat, reaction_rejects_local_identity_padding_nonfinite_and_truncation) {
  MeleeEvent e;
  e.revision = 1;
  e.source = 1;
  e.target = 2;
  e.sequence = 1;
  e.reaction = true;
  auto bytes = encode_melee(e);
  MeleeEvent decoded;
  KCD2S_CHECK_EQ(bytes.size(), std::size_t(200));
  KCD2S_CHECK(decode_melee(bytes, decoded));
  for (std::size_t n = 0; n < bytes.size(); ++n)
    KCD2S_CHECK(!decode_melee({bytes.begin(), bytes.begin() + n}, decoded));
  for (unsigned at : {0u, 4u, 8u, 15u, 37u, 40u, 66u, 72u, 119u, 156u, 159u}) {
    auto bad = e;
    bad.reaction_data[at] = 1;
    KCD2S_CHECK(encode_melee(bad).empty());
  }
  auto bad = e;
  float nan = std::numeric_limits<float>::quiet_NaN();
  std::memcpy(bad.reaction_data.data() + 80, &nan, 4);
  KCD2S_CHECK(encode_melee(bad).empty());
  bad = e;
  bad.reaction_data[172] = 2;
  KCD2S_CHECK(encode_melee(bad).empty());
  bad = e;
  bad.executor_is_target = true;
  KCD2S_CHECK(encode_melee(bad).empty());
  // A block or parry against a weapon: the sender's own item handle (+8..+15) is cleared, then the
  // reaction travels like one against fists.
  auto weapon = e;
  for (unsigned at = 8; at < 16; ++at)
    weapon.reaction_data[at] = std::uint8_t(0xA0 + at);
  weapon.reaction_data = portable_reaction_data(weapon.reaction_data);
  KCD2S_CHECK(!encode_melee(weapon).empty());
}
KCD2S_TEST(combat, reaction_uses_same_owner_world_and_epoch_fences) {
  Pair p;
  NativeReactionData reaction{};
  KCD2S_CHECK(!p.a->send_reaction(p.be, p.ae, reaction));
  KCD2S_CHECK(p.a->send_reaction(p.ae, p.be, reaction));
  p.a->tick(p.now);
  p.server->force_state(p.be);
  p.pump(12);
  KCD2S_CHECK(p.b->take_melee().empty());
  KCD2S_CHECK(p.a->send_reaction(p.ae, p.be, reaction));
  p.pump(12);
  auto events = p.b->take_melee();
  KCD2S_CHECK_EQ(events.size(), std::size_t(1));
  KCD2S_CHECK(events[0].reaction);
}

KCD2S_TEST(combat, native_defense_press_release_and_attack_order_survive_loss) {
  Pair p(true);
  using A = CombatAction;
  KCD2S_CHECK(p.a->send_combat_action(p.ae, p.be, {A::Target, 1, 1}));
  KCD2S_CHECK(p.a->send_combat_action(p.ae, p.be, {A::Block, 1, 1}));
  KCD2S_CHECK(p.a->send_combat_action(p.ae, p.be, {A::Block, 2, 0}));
  KCD2S_CHECK(p.a->send_combat_action(p.ae, p.be, {A::AttackMouse, 1, 1}));
  KCD2S_CHECK(p.a->send_melee(p.ae, p.be, cause()));
  p.pump(1200);
  auto events = p.b->take_melee();
  KCD2S_CHECK_EQ(events.size(), std::size_t(5));
  KCD2S_CHECK(events[0].input);
  KCD2S_CHECK(events[0].input_data.action == A::Target);
  KCD2S_CHECK_EQ(events[1].input_data.mode, std::uint8_t(1));
  KCD2S_CHECK_EQ(events[2].input_data.mode, std::uint8_t(2));
  KCD2S_CHECK(events[3].input_data.action == A::AttackMouse);
  KCD2S_CHECK(!events[4].input);
  for (unsigned i = 0; i < 5; ++i)
    KCD2S_CHECK_EQ(events[i].sequence, std::uint64_t(i + 1));
  p.pump(120);
  KCD2S_CHECK(p.b->take_melee().empty());
}
KCD2S_TEST(combat, attack_zone_precedes_the_attack_and_rejects_invalid_zones) {
  using A = CombatAction;
  // Only an integer zone 0..15 with mode 1 is a valid AttackZone.
  KCD2S_CHECK(valid_combat_action({A::AttackZone, 1, 5}));
  KCD2S_CHECK(valid_combat_action({A::AttackZone, 1, 0}));
  KCD2S_CHECK(!valid_combat_action({A::AttackZone, 2, 5}));
  KCD2S_CHECK(!valid_combat_action({A::AttackZone, 1, 16}));
  KCD2S_CHECK(!valid_combat_action({A::AttackZone, 1, -1}));
  KCD2S_CHECK(!valid_combat_action({A::AttackZone, 1, 2.5f}));
  KCD2S_CHECK(!valid_combat_action({A::AttackZone, 1, std::numeric_limits<float>::quiet_NaN()}));
  // An id past the last action.
  KCD2S_CHECK(!valid_combat_action({CombatAction(unsigned(A::Dodge) + 1), 1, 1}));
  Pair p(true);
  KCD2S_CHECK(p.a->send_combat_action(p.ae, p.be, {A::AttackZone, 1, 5}));
  KCD2S_CHECK(p.a->send_combat_action(p.ae, p.be, {A::AttackMouse, 1, 1}));
  KCD2S_CHECK(!p.a->send_combat_action(p.ae, p.be, {A::AttackZone, 1, 99}));
  p.pump(1200);
  auto events = p.b->take_melee();
  KCD2S_CHECK_EQ(events.size(), std::size_t(2));
  if (events.size() == 2) {
    KCD2S_CHECK(events[0].input_data.action == A::AttackZone);
    KCD2S_CHECK_EQ(events[0].input_data.value, 5.f);
    KCD2S_CHECK(events[1].input_data.action == A::AttackMouse);
  }
}
KCD2S_TEST(combat, perfect_block_carries_only_an_integer_variant_with_mode_1) {
  using A = CombatAction;
  KCD2S_CHECK(valid_combat_action({A::PerfectBlock, 1, 0}));
  KCD2S_CHECK(valid_combat_action({A::PerfectBlock, 1, 3}));
  KCD2S_CHECK(!valid_combat_action({A::PerfectBlock, 2, 3}));
  KCD2S_CHECK(!valid_combat_action({A::PerfectBlock, 1, 256}));
  KCD2S_CHECK(!valid_combat_action({A::PerfectBlock, 1, -1}));
  KCD2S_CHECK(!valid_combat_action({A::PerfectBlock, 1, 1.5f}));
}
KCD2S_TEST(combat, defence_timing_carries_whole_ms_up_to_5_s) {
  using A = CombatAction;
  KCD2S_CHECK(valid_combat_action({A::DefenceTiming, 1, 0}));
  KCD2S_CHECK(valid_combat_action({A::DefenceTiming, 1, 240}));
  KCD2S_CHECK(!valid_combat_action({A::DefenceTiming, 2, 240}));
  KCD2S_CHECK(!valid_combat_action({A::DefenceTiming, 1, 5001}));
  KCD2S_CHECK(!valid_combat_action({A::DefenceTiming, 1, 12.5f}));
}
KCD2S_TEST(combat, sync_perfect_block_carries_an_integer_request_with_mode_1_or_2) {
  using A = CombatAction;
  KCD2S_CHECK(valid_combat_action({A::SyncPerfectBlock, 1, 0}));
  KCD2S_CHECK(valid_combat_action({A::SyncPerfectBlock, 1, 255}));
  KCD2S_CHECK(valid_combat_action({A::SyncPerfectBlock, 2, 7}));
  KCD2S_CHECK(!valid_combat_action({A::SyncPerfectBlock, 4, 7}));
  KCD2S_CHECK(!valid_combat_action({A::SyncPerfectBlock, 1, 256}));
  KCD2S_CHECK(!valid_combat_action({A::SyncPerfectBlock, 1, 0.5f}));
}
KCD2S_TEST(combat, defence_guard_carries_a_whole_guard_zone_with_mode_1) {
  using A = CombatAction;
  KCD2S_CHECK(valid_combat_action({A::DefenceGuard, 1, 0}));
  KCD2S_CHECK(valid_combat_action({A::DefenceGuard, 1, 2}));
  KCD2S_CHECK(valid_combat_action({A::DefenceGuard, 1, 15}));
  KCD2S_CHECK(!valid_combat_action({A::DefenceGuard, 2, 2}));
  KCD2S_CHECK(!valid_combat_action({A::DefenceGuard, 1, 16}));
  KCD2S_CHECK(!valid_combat_action({A::DefenceGuard, 1, -1}));
  KCD2S_CHECK(!valid_combat_action({A::DefenceGuard, 1, 1.5f}));
}
KCD2S_TEST(combat, combat_model_carries_one_of_three_properties_with_a_whole_value) {
  using A = CombatAction;
  KCD2S_CHECK(valid_combat_action({A::CombatModel, 1, 0}));
  KCD2S_CHECK(valid_combat_action({A::CombatModel, 2, 2}));
  KCD2S_CHECK(valid_combat_action({A::CombatModel, 3, 24}));
  KCD2S_CHECK(valid_combat_action({A::CombatModel, 3, 255}));
  KCD2S_CHECK(!valid_combat_action({A::CombatModel, 0, 1}));
  KCD2S_CHECK(!valid_combat_action({A::CombatModel, 4, 1}));
  KCD2S_CHECK(!valid_combat_action({A::CombatModel, 1, 256}));
  KCD2S_CHECK(!valid_combat_action({A::CombatModel, 1, 0.5f}));
}
KCD2S_TEST(combat, defence_mode_carries_a_whole_value_up_to_255_with_mode_1) {
  using A = CombatAction;
  KCD2S_CHECK(valid_combat_action({A::DefenceMode, 1, 0}));
  KCD2S_CHECK(valid_combat_action({A::DefenceMode, 1, 24}));
  KCD2S_CHECK(valid_combat_action({A::DefenceMode, 1, 255}));
  KCD2S_CHECK(!valid_combat_action({A::DefenceMode, 2, 24}));
  KCD2S_CHECK(!valid_combat_action({A::DefenceMode, 1, 256}));
  KCD2S_CHECK(!valid_combat_action({A::DefenceMode, 1, 2.5f}));
}
KCD2S_TEST(combat, attack_refused_precedes_the_attack_input_it_marks) {
  using A = CombatAction;
  KCD2S_CHECK(valid_combat_action({A::AttackRefused, 1, 0}));
  KCD2S_CHECK(!valid_combat_action({A::AttackRefused, 2, 0}));
  KCD2S_CHECK(!valid_combat_action({A::AttackRefused, 1, 1}));
  Pair p(true);
  KCD2S_CHECK(p.a->send_combat_action(p.ae, p.be, {A::AttackRefused, 1, 0}));
  KCD2S_CHECK(p.a->send_combat_action(p.ae, p.be, {A::AttackMouse, 2, 0}));
  p.pump(1200);
  auto events = p.b->take_melee();
  KCD2S_CHECK_EQ(events.size(), std::size_t(2));
  if (events.size() == 2) {
    KCD2S_CHECK(events[0].input_data.action == A::AttackRefused);
    KCD2S_CHECK(events[1].input_data.action == A::AttackMouse);
  }
}
KCD2S_TEST(combat, hit_action_is_a_bare_mode_1_marker) {
  using A = CombatAction;
  KCD2S_CHECK(valid_combat_action({A::HitAction, 1, 0}));
  KCD2S_CHECK(!valid_combat_action({A::HitAction, 2, 0}));
  KCD2S_CHECK(!valid_combat_action({A::HitAction, 1, 1}));
  Pair p(true);
  KCD2S_CHECK(p.a->send_combat_action(p.ae, p.be, {A::HitAction, 1, 0}));
  p.pump(1200);
  auto events = p.b->take_melee();
  KCD2S_CHECK_EQ(events.size(), std::size_t(1));
  if (events.size() == 1) {
    KCD2S_CHECK(events[0].input_data.action == A::HitAction);
  }
}
KCD2S_TEST(combat, dodge_carries_its_direction_in_degrees_with_mode_1) {
  using A = CombatAction;
  // 0 right, 90 forward, 180 left, -90 back, and any analog direction between.
  KCD2S_CHECK(valid_combat_action({A::Dodge, 1, 0}));
  KCD2S_CHECK(valid_combat_action({A::Dodge, 1, 90}));
  KCD2S_CHECK(valid_combat_action({A::Dodge, 1, 180}));
  KCD2S_CHECK(valid_combat_action({A::Dodge, 1, -90}));
  KCD2S_CHECK(valid_combat_action({A::Dodge, 1, -135.5f}));
  KCD2S_CHECK(!valid_combat_action({A::Dodge, 2, 90}));
  KCD2S_CHECK(!valid_combat_action({A::Dodge, 1, 180.5f}));
  KCD2S_CHECK(!valid_combat_action({A::Dodge, 1, -181}));
  KCD2S_CHECK(!valid_combat_action({A::Dodge, 1, std::numeric_limits<float>::quiet_NaN()}));
  KCD2S_CHECK(!valid_combat_action({A::Dodge, 1, std::numeric_limits<float>::infinity()}));
}
KCD2S_TEST(combat, dodge_reaches_the_attacker_and_a_watcher_after_its_movement_keys) {
  using A = CombatAction;
  Pair p;
  p.add_observer();
  // The dodger holds left, its game makes the dodge, then the key goes up: every replica takes the
  // key before the dodge and the release after it.
  for (const auto target : {p.ae, p.ce}) {
    KCD2S_CHECK(p.b->send_combat_action(p.be, target, {A::MoveLeft, 1, 1}));
    KCD2S_CHECK(p.b->send_combat_action(p.be, target, {A::Dodge, 1, 180}));
    KCD2S_CHECK(p.b->send_combat_action(p.be, target, {A::MoveLeft, 2, 0}));
  }
  KCD2S_CHECK(!p.b->send_combat_action(p.be, p.ae, {A::Dodge, 1, 200}));
  p.pump(60);
  for (auto* peer : {p.a.get(), p.c.get()}) {
    const auto events = peer->take_melee();
    KCD2S_CHECK_EQ(events.size(), std::size_t(3));
    if (events.size() == 3) {
      KCD2S_CHECK(events[0].input_data.action == A::MoveLeft);
      KCD2S_CHECK(events[1].input_data.action == A::Dodge);
      KCD2S_CHECK_EQ(events[1].input_data.mode, std::uint8_t(1));
      KCD2S_CHECK_EQ(events[1].input_data.value, 180.f);
      KCD2S_CHECK(events[2].input_data.action == A::MoveLeft);
      KCD2S_CHECK_EQ(events[2].input_data.mode, std::uint8_t(2));
    }
  }
}
KCD2S_TEST(combat, riposte_precedes_the_attack_press_that_asked_for_it) {
  using A = CombatAction;
  KCD2S_CHECK(valid_combat_action({A::Riposte, 1, 0}));
  KCD2S_CHECK(!valid_combat_action({A::Riposte, 2, 0}));
  KCD2S_CHECK(!valid_combat_action({A::Riposte, 1, 1}));
  Pair p(true);
  KCD2S_CHECK(p.a->send_combat_action(p.ae, p.be, {A::Riposte, 1, 0}));
  KCD2S_CHECK(p.a->send_combat_action(p.ae, p.be, {A::AttackMouse, 1, 1}));
  p.pump(1200);
  auto events = p.b->take_melee();
  KCD2S_CHECK_EQ(events.size(), std::size_t(2));
  if (events.size() == 2) {
    KCD2S_CHECK(events[0].input_data.action == A::Riposte);
    KCD2S_CHECK(events[1].input_data.action == A::AttackMouse);
  }
}
KCD2S_TEST(combat, target_other_carries_a_whole_network_id_with_mode_1) {
  using A = CombatAction;
  KCD2S_CHECK(valid_combat_action({A::TargetOther, 1, 0}));
  KCD2S_CHECK(valid_combat_action({A::TargetOther, 1, 4113}));
  KCD2S_CHECK(valid_combat_action({A::TargetOther, 1, kMaxCombatTargetId}));
  KCD2S_CHECK(!valid_combat_action({A::TargetOther, 2, 4113}));
  KCD2S_CHECK(!valid_combat_action({A::TargetOther, 1, -1}));
  KCD2S_CHECK(!valid_combat_action({A::TargetOther, 1, 4113.5f}));
  KCD2S_CHECK(!valid_combat_action({A::TargetOther, 1, kMaxCombatTargetId + 2}));
}
KCD2S_TEST(combat, clinch_handshake_accepts_only_its_four_modes_of_a_known_kind) {
  using A = CombatAction;
  // Mode 1 asks the other client to start the clinch, mode 2 confirms it, mode 3 tells a watcher
  // that it started, mode 4 refuses it; kind 0 or 1 only.
  KCD2S_CHECK(valid_combat_action({A::Clinch, 1, 0}));
  KCD2S_CHECK(valid_combat_action({A::Clinch, 1, 1}));
  KCD2S_CHECK(valid_combat_action({A::Clinch, 2, 1}));
  KCD2S_CHECK(valid_combat_action({A::Clinch, 3, 0}));
  KCD2S_CHECK(valid_combat_action({A::Clinch, 4, 1}));
  KCD2S_CHECK(!valid_combat_action({A::Clinch, 0, 0}));
  KCD2S_CHECK(!valid_combat_action({A::Clinch, 5, 0}));
  KCD2S_CHECK(!valid_combat_action({A::Clinch, 1, 2}));
  KCD2S_CHECK(!valid_combat_action({A::Clinch, 4, 2}));
  KCD2S_CHECK(!valid_combat_action({A::Clinch, 1, 0.5f}));
  KCD2S_CHECK(!valid_combat_action({A::Clinch, 2, std::numeric_limits<float>::quiet_NaN()}));
  Pair p(true);
  KCD2S_CHECK(p.a->send_combat_action(p.ae, p.be, {A::Clinch, 1, 1}));
  KCD2S_CHECK(!p.a->send_combat_action(p.ae, p.be, {A::Clinch, 5, 1}));
  p.pump(1200);
  auto events = p.b->take_melee();
  KCD2S_CHECK_EQ(events.size(), std::size_t(1));
  if (events.size() == 1) {
    KCD2S_CHECK(events[0].input_data.action == A::Clinch);
    KCD2S_CHECK_EQ(events[0].input_data.mode, std::uint8_t(1));
    KCD2S_CHECK_EQ(events[0].input_data.value, 1.f);
  }
}
KCD2S_TEST(combat, a_started_clinch_reaches_the_watcher_before_the_push_input) {
  using A = CombatAction;
  Pair p;
  p.add_observer();
  // The initiator's game started the clinch (after the other client's accept), then its F press
  // and release: the watcher's replicas hold the clinch before the push is replayed on them.
  KCD2S_CHECK(p.a->send_combat_action(p.ae, p.ce, {A::Clinch, 3, 0}));
  KCD2S_CHECK(p.a->send_combat_action(p.ae, p.ce, {A::Special, 1, 1}));
  KCD2S_CHECK(p.a->send_combat_action(p.ae, p.ce, {A::Special, 2, 0}));
  p.pump(60);
  const auto events = p.c->take_melee();
  KCD2S_CHECK_EQ(events.size(), std::size_t(3));
  if (events.size() == 3) {
    KCD2S_CHECK(events[0].input_data.action == A::Clinch);
    KCD2S_CHECK_EQ(events[0].input_data.mode, std::uint8_t(3));
    KCD2S_CHECK_EQ(events[0].input_data.value, 0.f);
    KCD2S_CHECK(events[1].input_data.action == A::Special);
    KCD2S_CHECK_EQ(events[1].input_data.mode, std::uint8_t(1));
    KCD2S_CHECK(events[2].input_data.action == A::Special);
    KCD2S_CHECK_EQ(events[2].input_data.mode, std::uint8_t(2));
  }
  // Only the watcher it was sent to gets it.
  KCD2S_CHECK(p.b->take_melee().empty());
}
KCD2S_TEST(combat, native_input_rejects_forged_owner_epoch_and_malformed_values) {
  using A = CombatAction;
  Pair p;
  CombatActionData data{A::Block, 1, 1};
  KCD2S_CHECK(!p.a->send_combat_action(p.be, p.ae, data));
  p.mutate->change = [](MeleeEvent& e) { e.source_epoch++; };
  KCD2S_CHECK(p.a->send_combat_action(p.ae, p.be, data));
  p.pump(12);
  KCD2S_CHECK(p.b->take_melee().empty());
  MeleeEvent e;
  e.revision = 1;
  e.source = 1;
  e.target = 2;
  e.sequence = 1;
  e.input = true;
  e.input_data = data;
  auto bytes = encode_melee(e);
  MeleeEvent decoded;
  KCD2S_CHECK_EQ(bytes.size(), std::size_t(33));
  KCD2S_CHECK(decode_melee(bytes, decoded));
  for (unsigned n = 0; n < bytes.size(); ++n)
    KCD2S_CHECK(!decode_melee({bytes.begin(), bytes.begin() + n}, decoded));
  for (auto invalid : {0, 25, 255}) {
    auto bad = e;
    bad.input_data.action = A(invalid);
    KCD2S_CHECK(encode_melee(bad).empty());
  }
  auto bad = e;
  bad.input_data.mode = 3;
  KCD2S_CHECK(encode_melee(bad).empty());
  bad = e;
  bad.input_data.value = std::numeric_limits<float>::quiet_NaN();
  KCD2S_CHECK(encode_melee(bad).empty());
  bad = e;
  bad.executor_is_target = true;
  KCD2S_CHECK(encode_melee(bad).empty());
  bad = e;
  bad.reaction = true;
  KCD2S_CHECK(encode_melee(bad).empty());
}

KCD2S_TEST(combat, native_preview_destroy_and_allocation_reuse_do_not_relay_damage) {
  local::NativeEventRelay relay;
  MeleeEvent preview;
  preview.source = 1;
  preview.target = 2;
  preview.data[84] = 3;
  relay.stage(0x1234, preview);
  KCD2S_CHECK(relay.destroy(0x1234));
  KCD2S_CHECK(!relay.submit(0x1234));
  auto committed = preview;
  committed.data[84] = 4;
  relay.stage(0x1234, committed);
  auto hit = relay.submit(0x1234);
  KCD2S_CHECK(hit);
  KCD2S_CHECK_EQ(hit->data[84], 4);
  KCD2S_CHECK(!relay.submit(0x1234));
  KCD2S_CHECK(!relay.destroy(0x1234));
}
KCD2S_TEST(combat, native_commit_interleaving_and_world_reset_fence_pending_events) {
  local::NativeEventRelay relay;
  MeleeEvent a, b;
  a.source = 1;
  a.target = 2;
  b.source = 2;
  b.target = 1;
  relay.stage(11, a);
  relay.stage(22, b);
  KCD2S_CHECK(!relay.submit(0));
  KCD2S_CHECK(!relay.submit(33));
  auto hit = relay.submit(22);
  KCD2S_CHECK(hit);
  KCD2S_CHECK_EQ(hit->source, 2u);
  relay.clear();
  KCD2S_CHECK(!relay.submit(11));
}

KCD2S_TEST(combat, native_clinch_exit_movement_and_special_input_survive_loss) {
  Pair p(true);
  const CombatAction actions[] = {CombatAction::MoveForward, CombatAction::MoveBack,
                                  CombatAction::MoveLeft, CombatAction::MoveRight,
                                  CombatAction::Special};
  for (auto action : actions) {
    KCD2S_CHECK(p.a->send_combat_action(p.ae, p.be, {action, 1, 1.f}));
    KCD2S_CHECK(p.a->send_combat_action(p.ae, p.be, {action, 2, 0.f}));
  }
  p.pump(1200);
  auto received = p.b->take_melee();
  KCD2S_CHECK_EQ(received.size(), std::size_t(10));
  for (std::size_t i = 0; i < received.size(); ++i) {
    KCD2S_CHECK(received[i].input);
    KCD2S_CHECK_EQ(unsigned(received[i].input_data.action), unsigned(actions[i / 2]));
    KCD2S_CHECK_EQ(received[i].input_data.mode, i % 2 ? 2 : 1);
  }
}

KCD2S_TEST(combat, original_sync_hit_selection_precedes_release_exactly_once_under_loss) {
  Pair p(true);
  NativeSyncHitData data{};
  data[0] = 0xb4;
  data[8] = 0x0f;
  data[56] = 1;
  data[61] = 0x7d;
  std::uint32_t context = 256;
  std::memcpy(data.data() + 57, &context, 4);
  for (unsigned n = 0; n < 8; ++n) {
    data[20] = std::uint8_t(n);
    KCD2S_CHECK(p.a->send_sync_hit(p.ae, p.be, data));
    KCD2S_CHECK(p.a->send_combat_action(p.ae, p.be, {CombatAction::AttackMouse, 2, 0}));
  }
  p.pump(1200);
  auto received = p.b->take_melee();
  KCD2S_CHECK_EQ(received.size(), std::size_t(16));
  for (unsigned n = 0; n < 8; ++n) {
    KCD2S_CHECK(received[2 * n].sync_hit);
    KCD2S_CHECK_EQ(received[2 * n].sync_hit_data[20], std::uint8_t(n));
    KCD2S_CHECK(received[2 * n + 1].input);
    KCD2S_CHECK_EQ(received[2 * n + 1].input_data.mode, 2);
  }
  p.pump(120);
  KCD2S_CHECK(p.b->take_melee().empty());
  KCD2S_CHECK(!p.a->send_sync_hit(p.be, p.ae, data));
}
KCD2S_TEST(combat, sync_hit_selection_rejects_truncation_invalid_context_and_mixed_payloads) {
  MeleeEvent e;
  e.revision = 1;
  e.source = 1;
  e.target = 2;
  e.sequence = 1;
  e.sync_hit = true;
  e.sync_hit_data[0] = 1;
  e.sync_hit_data[61] = 1;
  auto bytes = encode_melee(e);
  MeleeEvent out;
  KCD2S_CHECK_EQ(bytes.size(), std::size_t(148));
  KCD2S_CHECK(decode_melee(bytes, out));
  KCD2S_CHECK(out.sync_hit_data == e.sync_hit_data);
  for (std::size_t n = 0; n < bytes.size(); ++n)
    KCD2S_CHECK(!decode_melee({bytes.begin(), bytes.begin() + n}, out));
  auto bad = e;
  bad.sync_hit_data[56] = 2;
  KCD2S_CHECK(encode_melee(bad).empty());
  bad = e;
  bad.sync_hit_data[57] = 1;
  KCD2S_CHECK(encode_melee(bad).empty());
  bad = e;
  bad.reaction = true;
  KCD2S_CHECK(encode_melee(bad).empty());
  bad = e;
  bad.input = true;
  KCD2S_CHECK(encode_melee(bad).empty());
  bad = e;
  std::fill(bad.sync_hit_data.begin() + 61, bad.sync_hit_data.begin() + 117, std::uint8_t(0));
  KCD2S_CHECK(encode_melee(bad).empty());
  bad = e;
  bad.sync_hit_data = {};
  KCD2S_CHECK(encode_melee(bad).empty());
}

KCD2S_TEST(combat, victim_native_results_return_to_attacker_once_under_loss) {
  Pair p(true);
  NativeReactionData reaction{};
  for (unsigned n = 0; n < 8; ++n) {
    auto data = cause();
    data[84] = std::uint8_t(n);
    KCD2S_CHECK(p.b->send_melee(p.ae, p.be, data, false, true));
    reaction[52] = std::uint8_t(n);
    KCD2S_CHECK(p.b->send_reaction(p.ae, p.be, reaction, true));
  }
  p.pump(1200);
  auto received = p.a->take_melee();
  KCD2S_CHECK_EQ(received.size(), std::size_t(16));
  for (unsigned n = 0; n < 8; ++n) {
    KCD2S_CHECK(received[2 * n].authority_is_target);
    KCD2S_CHECK_EQ(received[2 * n].data[84], std::uint8_t(n));
    KCD2S_CHECK(received[2 * n + 1].authority_is_target && received[2 * n + 1].reaction);
    KCD2S_CHECK_EQ(received[2 * n + 1].reaction_data[52], std::uint8_t(n));
  }
  KCD2S_CHECK(p.b->take_melee().empty());
  p.pump(120);
  KCD2S_CHECK(p.a->take_melee().empty());
  KCD2S_CHECK(!p.a->send_melee(p.ae, p.be, cause(), false, true));
  KCD2S_CHECK(!p.a->send_reaction(p.ae, p.be, reaction, true));
}
KCD2S_TEST(combat, victim_authority_cannot_forge_attacker_inputs_or_pair_selection) {
  MeleeEvent e;
  e.revision = 1;
  e.source = 1;
  e.target = 2;
  e.sequence = 1;
  e.authority_is_target = true;
  e.input = true;
  e.input_data = {CombatAction::Block, 1, 1};
  KCD2S_CHECK(encode_melee(e).empty());
  e.input = false;
  e.sync_hit = true;
  e.sync_hit_data[0] = 1;
  e.sync_hit_data[61] = 1;
  KCD2S_CHECK(encode_melee(e).empty());
  Pair p;
  p.mutate->change = [](MeleeEvent& value) { value.authority_is_target = true; };
  KCD2S_CHECK(p.a->send_melee(p.ae, p.be, cause()));
  p.pump(120);
  KCD2S_CHECK(p.b->take_melee().empty());
  KCD2S_CHECK(p.a->take_melee().empty());
}

KCD2S_TEST(combat, native_contact_and_committed_effects_order_once_under_loss) {
  Pair p(true);
  NativeContactData contact{};
  NativeEffectRecord health;
  health.kind = 1;
  health.soul_role = 2;
  float delta = -3.864f;
  std::memcpy(health.value.data() + 4, &delta, 4);
  for (unsigned i = 0; i < 8; ++i) {
    contact[308] = std::uint8_t(i);
    KCD2S_CHECK(p.a->send_native_contact(p.ae, p.be, contact));
  }
  p.pump(1600);
  auto contacts = p.b->take_melee();
  KCD2S_CHECK_EQ(contacts.size(), std::size_t(8));
  for (unsigned i = 0; i < contacts.size(); ++i) {
    KCD2S_CHECK(contacts[i].contact);
    KCD2S_CHECK_EQ(contacts[i].contact_data[308], i);
    health.value[12] = std::uint8_t(i);
    KCD2S_CHECK(p.b->send_native_effects(p.ae, p.be, cause(), {health}));
  }
  p.pump(1600);
  auto results = p.a->take_melee();
  KCD2S_CHECK_EQ(results.size(), std::size_t(8));
  for (unsigned i = 0; i < results.size(); ++i) {
    KCD2S_CHECK(results[i].effects && results[i].authority_is_target);
    KCD2S_CHECK_EQ(results[i].effect_data[0].value[12], i);
  }
  p.pump(400);
  KCD2S_CHECK(p.a->take_melee().empty());
  KCD2S_CHECK(p.b->take_melee().empty());
}

KCD2S_TEST(combat, armed_committed_effects_relay_only_equipment_tokens) {
  Pair p(true);
  auto native = cause();
  melee_set_u64(native, 120, 0x020000000000a490);
  melee_set_u64(native, 128, 0x020000000000a491);
  melee_set_u64(native, 136, 0x020000000000a485);
  const CombatEquipmentTokens tokens{420, 421, 77};
  const auto portable = portable_committed_melee_data(native, tokens);
  KCD2S_CHECK(valid_committed_melee_data(portable));
  KCD2S_CHECK(!valid_melee_data(portable));
  KCD2S_CHECK(!valid_committed_melee_data(native));

  NativeEffectRecord health;
  health.kind = 1;
  health.soul_role = 2;
  const float delta = -14.29344f;
  std::memcpy(health.value.data() + 4, &delta, sizeof(delta));
  KCD2S_CHECK(p.b->send_native_effects(p.ae, p.be, portable, {health}));
  KCD2S_CHECK(!p.b->send_native_effects(p.ae, p.be, native, {health}));
  p.pump(1600);
  const auto received = p.a->take_melee();
  KCD2S_CHECK_EQ(received.size(), std::size_t(1));
  KCD2S_CHECK(received.front().effects);
  KCD2S_CHECK(received.front().data == portable);
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    KCD2S_CHECK_EQ(melee_u64(received.front().data, kCombatEquipmentOffsets[index]),
                   std::uint64_t(tokens[index]));
  }

  auto encoded = encode_melee(received.front());
  KCD2S_CHECK_EQ(encoded[3], std::uint8_t(8));
  MeleeEvent decoded;
  KCD2S_CHECK(decode_melee(encoded, decoded));
  encoded[3] = 6;
  KCD2S_CHECK(!decode_melee(encoded, decoded));
  encoded[3] = 8;
  encoded[27 + 120 + 4] = 1;
  KCD2S_CHECK(!decode_melee(encoded, decoded));
  p.pump(400);
  KCD2S_CHECK(p.a->take_melee().empty());
}

KCD2S_TEST(combat, legacy_committed_effects_keep_the_unarmed_identity_contract) {
  MeleeEvent event;
  event.revision = 1;
  event.source = 1;
  event.target = 2;
  event.sequence = 1;
  event.authority_is_target = true;
  event.effects = true;
  event.data = cause();
  NativeEffectRecord health;
  health.kind = 1;
  health.soul_role = 2;
  event.effect_data = {health};
  auto legacy = encode_melee(event);
  legacy[3] = 6;
  MeleeEvent decoded;
  KCD2S_CHECK(decode_melee(legacy, decoded));
  KCD2S_CHECK(decoded.data == event.data);
  legacy[27 + 128] = 2;
  KCD2S_CHECK(!decode_melee(legacy, decoded));
}

KCD2S_TEST(combat, native_effects_and_contacts_reject_spoof_malformed_and_epoch) {
  NativeEffectRecord health;
  health.kind = 1;
  health.soul_role = 2;
  NativeEffects effects{health};
  NativeContactData contact{};
  Pair p;
  KCD2S_CHECK(!p.a->send_native_effects(p.ae, p.be, cause(), effects));
  KCD2S_CHECK(!p.b->send_native_contact(p.ae, p.be, contact));
  MeleeEvent event;
  event.revision = 1;
  event.source = 1;
  event.target = 2;
  event.sequence = 1;
  event.effects = true;
  event.authority_is_target = true;
  event.effect_data = effects;
  auto good = encode_melee(event);
  MeleeEvent decoded;
  KCD2S_CHECK(decode_melee(good, decoded));
  auto bad = good;
  bad[171] = 17;
  KCD2S_CHECK(!decode_melee(bad, decoded));
  bad = good;
  bad[26] = 0;
  KCD2S_CHECK(!decode_melee(bad, decoded));
  bad = good;
  bad[173] = 3;
  KCD2S_CHECK(!decode_melee(bad, decoded));
  bad = good;
  bad.pop_back();
  KCD2S_CHECK(!decode_melee(bad, decoded));
  contact[320] = 9;
  KCD2S_CHECK(!valid_contact_data(contact));
  contact[320] = 0;
  auto nan = std::numeric_limits<float>::quiet_NaN();
  std::memcpy(contact.data() + 449, &nan, 4);
  KCD2S_CHECK(!valid_contact_data(contact));
  contact = {};
  KCD2S_CHECK(p.b->send_native_effects(p.ae, p.be, cause(), effects));
  p.b->tick(p.now);
  p.server->force_state(p.be);
  p.pump(30);
  KCD2S_CHECK(p.a->take_melee().empty());
  p.mutate->change = [](MeleeEvent& e) { e.authority_is_target = true; };
  KCD2S_CHECK(p.a->send_native_contact(p.ae, p.be, contact));
  p.pump(80);
  KCD2S_CHECK(p.b->take_melee().empty());
}

KCD2S_TEST(combat, hits_do_not_cross_dimensions) {
  Pair p;
  // Baseline: a hit lands while both avatars share the global world.
  KCD2S_CHECK(p.a->send_melee(p.ae, p.be, cause()));
  p.pump(8);
  KCD2S_CHECK_EQ(p.b->take_melee().size(), std::size_t(1));
  // Separate the avatars into two different numbered worlds, then fire immediately.
  // The construct bookkeeping (`confirmed`) still holds both peer avatars for the
  // first ticks after the change, so only the server's live scope re-check can stop
  // the hit from crossing the virtual-world boundary. set_dimension leaves the
  // forced-state epoch untouched, so the epoch fence is not what rejects it here.
  p.server->set_dimension(p.ae, 7);
  p.server->set_dimension(p.be, 9);
  KCD2S_CHECK(p.a->send_melee(p.ae, p.be, cause()));
  p.pump(16);
  KCD2S_CHECK(p.b->take_melee().empty());
  // Reunite them in one world; the victim avatar is reconstructed and combat flows.
  p.server->set_dimension(p.ae, 7);
  p.server->set_dimension(p.be, 7);
  p.pump(120);
  KCD2S_CHECK(p.a->send_melee(p.ae, p.be, cause()));
  p.pump(16);
  KCD2S_CHECK_EQ(p.b->take_melee().size(), std::size_t(1));
}

KCD2S_TEST(combat, native_stamina_mode_and_injured_buff_are_preserved) {
  Pair p(true);
  NativeEffectRecord stamina;
  stamina.kind = 2;
  stamina.soul_role = 2;
  unsigned attribute = 1;
  float delta = -12.5f;
  std::memcpy(stamina.value.data(), &attribute, 4);
  std::memcpy(stamina.value.data() + 4, &delta, 4);
  stamina.value[8] = 2;
  NativeEffectRecord buff;
  buff.kind = 3;
  buff.soul_role = 2;
  buff.value[0] = 1;
  buff.value[32] = 2;
  float duration = 0.75f;
  std::memcpy(buff.value.data() + 36, &duration, 4);
  buff.value[44] = 1;
  buff.value[45] = 1;
  KCD2S_CHECK(p.b->send_native_effects(p.ae, p.be, cause(), {stamina, buff}));
  p.pump(1000);
  auto result = p.a->take_melee();
  KCD2S_CHECK_EQ(result.size(), std::size_t(1));
  KCD2S_CHECK_EQ(result[0].effect_data[0].value[8], 2);
  KCD2S_CHECK_EQ(result[0].effect_data[1].value[32], 2);
  KCD2S_CHECK_EQ(result[0].effect_data[1].value[45], 1);
  buff.value[45] = 2;
  KCD2S_CHECK(!p.b->send_native_effects(p.ae, p.be, cause(), {stamina, buff}));
}

KCD2S_TEST(combat, attacker_item_wear_travels_with_its_class_and_change) {
  Pair p(true);
  NativeEffectRecord wear;
  wear.kind = 4;
  wear.soul_role = 1;
  for (unsigned i = 0; i < 16; ++i)
    wear.value[i] = std::uint8_t(0xA0 + i);
  float change = -0.0004f;
  std::memcpy(wear.value.data() + 16, &change, 4);
  KCD2S_CHECK(p.b->send_native_effects(p.ae, p.be, cause(), {wear}));
  p.pump(1000);
  auto result = p.a->take_melee();
  KCD2S_CHECK_EQ(result.size(), std::size_t(1));
  if (result.size() == 1) {
    KCD2S_CHECK_EQ(result[0].effect_data[0].kind, 4);
    KCD2S_CHECK_EQ(result[0].effect_data[0].value[15], 0xAF);
    float received = 0;
    std::memcpy(&received, result[0].effect_data[0].value.data() + 16, 4);
    KCD2S_CHECK_EQ(received, change);
  }
  change = 1.5f;
  std::memcpy(wear.value.data() + 16, &change, 4);
  KCD2S_CHECK(!valid_native_effects({wear}));
  change = -0.25f;
  std::memcpy(wear.value.data() + 16, &change, 4);
  wear.value[20] = 1;
  KCD2S_CHECK(!valid_native_effects({wear}));
}

KCD2S_TEST(combat, attacker_item_break_travels_with_its_class_only) {
  Pair p(true);
  NativeEffectRecord broken;
  broken.kind = 5;
  broken.soul_role = 1;
  for (unsigned i = 0; i < 16; ++i)
    broken.value[i] = std::uint8_t(0xB0 + i);
  KCD2S_CHECK(p.b->send_native_effects(p.ae, p.be, cause(), {broken}));
  p.pump(1000);
  auto result = p.a->take_melee();
  KCD2S_CHECK_EQ(result.size(), std::size_t(1));
  if (result.size() == 1) {
    KCD2S_CHECK_EQ(result[0].effect_data[0].kind, 5);
    KCD2S_CHECK_EQ(result[0].effect_data[0].value[15], 0xBF);
  }
  broken.value[16] = 1;
  KCD2S_CHECK(!valid_native_effects({broken}));
  broken.value[16] = 0;
  broken.kind = 6;
  KCD2S_CHECK(!valid_native_effects({broken}));
}

// Player.startFight and stopFight: each client of the pair hears the other as its opponent.
KCD2S_TEST(combat, start_and_cancel_reach_each_client_of_the_pair) {
  Pair pair;
  KCD2S_CHECK(pair.server->request_combat_start(pair.ae, pair.be));
  KCD2S_CHECK(pair.server->request_combat_start(pair.be, pair.ae));
  KCD2S_CHECK(!pair.server->request_combat_start(pair.ae, pair.ae));
  KCD2S_CHECK(!pair.server->request_combat_start(pair.ae, 0));
  pair.pump(5);
  KCD2S_CHECK(pair.a->take_combat_starts() == std::vector<std::uint32_t>{std::uint32_t(pair.be)});
  KCD2S_CHECK(pair.b->take_combat_starts() == std::vector<std::uint32_t>{std::uint32_t(pair.ae)});
  KCD2S_CHECK(pair.a->take_combat_starts().empty());
  KCD2S_CHECK(pair.server->request_combat_cancel(pair.ae, pair.be));
  pair.pump(5);
  KCD2S_CHECK(pair.a->take_combat_cancels() == std::vector<std::uint32_t>{std::uint32_t(pair.be)});
  KCD2S_CHECK(pair.a->take_combat_starts().empty());
}

// playerHarm: harm the player's game holds reaches the server only while the rules hold it.
KCD2S_TEST(combat, harm_waits_for_a_ruling_only_while_the_rules_say_so) {
  Pair pair;
  PlayerHarm fall{0x80000001u, 30.0f, 7, 55, 0};
  KCD2S_CHECK(pair.a->send_player_harm(fall));
  pair.pump(5);
  KCD2S_CHECK(pair.server->take_player_harms().empty());
  KCD2S_CHECK(!pair.a->harm_rules());
  pair.server->set_hit_rules(false, true);
  pair.pump(5);
  KCD2S_CHECK(pair.a->harm_rules() && !pair.a->hit_rules());
  KCD2S_CHECK(pair.a->send_player_harm(fall));
  pair.pump(5);
  const auto harms = pair.server->take_player_harms();
  KCD2S_CHECK_EQ(harms.size(), std::size_t(1));
  if (harms.size() == 1) {
    KCD2S_CHECK(harms[0].first == pair.ae);
    KCD2S_CHECK(harms[0].second == fall);
  }
  // The ruling comes back as a HIT_VERDICT for that request.
  HitVerdict verdict;
  verdict.request = fall.request;
  verdict.ruling = HitRuling::Set;
  verdict.value = 5.0f;
  KCD2S_CHECK(pair.server->send_hit_verdict(pair.ae, verdict));
  pair.pump(5);
  const auto verdicts = pair.a->take_hit_verdicts();
  KCD2S_CHECK(verdicts.size() == 1 && verdicts[0] == verdict);
}
