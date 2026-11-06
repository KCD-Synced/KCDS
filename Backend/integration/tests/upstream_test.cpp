#include "../local/rider_state.hpp"
#include "../local/movement.hpp"
#include <cstddef>
#include <limits>
#include <memory>
#include <variant>
#include <vector>

#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/control.hpp"
#include "kcd2s/integration/server.hpp"
#include "kcd2s/replication/schema.hpp"
#include "kcd2s/replication/world.hpp"
#include "kcd2s/testkit.hpp"
#include "kcd2s/transport/datagram.hpp"

using namespace kcd2s::integration;
using kcd2s::core::EntityId;
using kcd2s::core::FieldId;
using kcd2s::core::Quat;
using kcd2s::replication::Authority;
using kcd2s::replication::FieldKind;
using kcd2s::replication::FieldValue;
using kcd2s::replication::Priority;
using kcd2s::replication::SchemaRegistry;
using kcd2s::replication::Vec3;
using kcd2s::transport::make_sim_pair;
using kcd2s::transport::SimLink;

static SchemaRegistry schema() {
  SchemaRegistry reg;
  reg.define("Avatar")
      .field("position", FieldKind::Vec3, Priority::High, Authority::Owner)  // field 0
      .field("health", FieldKind::F32, Priority::Med, Authority::Server);    // field 1
  return reg;
}

struct Peer {
  std::unique_ptr<SimLink> s, c;
  std::unique_ptr<ClientPeer> peer;
  PeerId pid = 0;
};

static Peer join(Server& srv, const SchemaRegistry& reg, EntityId viewer) {
  Peer p;
  auto pr = make_sim_pair({}, {});
  p.s = std::move(pr.first);
  p.c = std::move(pr.second);
  p.pid = srv.add_client(p.s.get(), viewer, 1.0e6f);
  p.peer = std::make_unique<ClientPeer>(reg, p.c.get());
  return p;
}

KCD2S_TEST(upstream, owner_input_propagates) {
  SchemaRegistry reg = schema();
  Server server(reg);
  auto& w = server.world();
  const auto avatar_t = reg.get("Avatar")->id;

  w.set_tick(1);
  const EntityId avatarA = w.spawn(avatar_t);
  w.write(avatarA, 0, Vec3{0, 0, 0});

  Peer a = join(server, reg, avatarA);
  Peer b = join(server, reg, avatarA);  // B just observes
  server.set_owner(avatarA, a.pid);

  kcd2s::core::Tick frame = 1;
  auto pump = [&](int n) {
    for (int i = 0; i < n; ++i) {
      w.set_tick(++frame);  // advance the sim tick each frame (as a real server does)
      server.tick(0.0);
      a.peer->tick(0.0);
      b.peer->tick(0.0);
    }
  };
  pump(3);
  KCD2S_CHECK(a.peer->owns(avatarA));  // learned via SET_OWNER

  // Owner authors its position upstream; server applies it, then it replicates to B.
  a.peer->send_input(avatarA, {{FieldId{0}, FieldValue{Vec3{100, 0, 50}}}});
  pump(4);

  const auto* sp = w.slot(avatarA);
  KCD2S_CHECK((sp->position == Vec3{100, 0, 50}));  // server accepted owner write
  const auto* bmir = b.peer->mirror().get(avatarA);
  KCD2S_CHECK(bmir != nullptr);
  KCD2S_CHECK((std::get<Vec3>(bmir->fields[0]) == Vec3{100, 0, 50}));  // reached observer B
}

KCD2S_TEST(upstream, non_owner_forged_write_rejected) {
  SchemaRegistry reg = schema();
  Server server(reg);
  auto& w = server.world();
  const auto avatar_t = reg.get("Avatar")->id;

  w.set_tick(1);
  const EntityId avatarA = w.spawn(avatar_t);
  w.write(avatarA, 0, Vec3{7, 7, 7});

  Peer a = join(server, reg, avatarA);
  Peer b = join(server, reg, avatarA);
  server.set_owner(avatarA, a.pid);

  kcd2s::core::Tick frame = 1;
  auto pump = [&](int n) {
    for (int i = 0; i < n; ++i) {
      w.set_tick(++frame);  // advance the sim tick each frame (as a real server does)
      server.tick(0.0);
      a.peer->tick(0.0);
      b.peer->tick(0.0);
    }
  };
  pump(3);  // B now sees avatarA in its mirror (so it can forge an upstream for it)

  // B does NOT own avatarA but tries to move it.
  b.peer->send_input(avatarA, {{FieldId{0}, FieldValue{Vec3{-999, -999, -999}}}});
  pump(4);

  const auto* sp = w.slot(avatarA);
  KCD2S_CHECK((sp->position == Vec3{7, 7, 7}));  // forged write rejected; position unchanged
}

KCD2S_TEST(upstream, forced_state_fences_stale_owner_input) {
  SchemaRegistry reg = schema();
  Server server(reg);
  auto& w = server.world();
  const auto avatar_t = reg.get("Avatar")->id;

  w.set_tick(1);
  const EntityId avatarA = w.spawn(avatar_t);
  w.write(avatarA, 0, Vec3{0, 0, 0});

  Peer a = join(server, reg, avatarA);
  server.set_owner(avatarA, a.pid);
  kcd2s::core::Tick frame = 1;
  auto step = [&] {
    w.set_tick(++frame);
    a.peer->tick(0.0);
    server.tick(0.0);
  };
  step();  // A learns SET_OWNER (epoch 0)
  step();

  // Epoch-0 input is accepted.
  a.peer->send_input(avatarA, {{FieldId{0}, FieldValue{Vec3{10, 0, 0}}}});
  step();
  KCD2S_CHECK((w.slot(avatarA)->position == Vec3{10, 0, 0}));

  // Server forces state (epoch 0 -> 1); A has NOT yet received the FORCE_STATE event.
  server.force_state(avatarA);
  KCD2S_CHECK(w.slot(avatarA)->stateEpoch == 1);

  // A sends input still stamped epoch 0 -> must be rejected (stale forced state).
  a.peer->send_input(avatarA, {{FieldId{0}, FieldValue{Vec3{-1, -1, -1}}}});
  a.peer->tick(0.0);  // flush the stale input (A still at epoch 0)
  server.tick(0.0);   // server rejects it, and flushes FORCE_STATE to A
  KCD2S_CHECK((w.slot(avatarA)->position == Vec3{10, 0, 0}));  // unchanged

  // A adopts the new epoch, then its input is accepted again.
  a.peer->tick(0.0);  // receive FORCE_STATE -> epoch 1
  a.peer->send_input(avatarA, {{FieldId{0}, FieldValue{Vec3{20, 0, 0}}}});
  step();
  KCD2S_CHECK((w.slot(avatarA)->position == Vec3{20, 0, 0}));
}

KCD2S_TEST(upstream, delegation_reassigns_owner_on_disconnect) {
  SchemaRegistry reg = schema();
  Server server(reg);
  auto& w = server.world();
  const auto avatar_t = reg.get("Avatar")->id;

  w.set_tick(1);
  const EntityId avatarA = w.spawn(avatar_t);
  w.write(avatarA, 0, Vec3{10, 0, 10});
  const EntityId viewerB = w.spawn(avatar_t);
  w.write(viewerB, 0, Vec3{12, 0, 10});  // B's viewer sits near avatarA -> B is the natural heir

  Peer a = join(server, reg, avatarA);
  Peer b = join(server, reg, viewerB);
  server.set_owner(avatarA, a.pid);

  kcd2s::core::Tick frame = 1;
  double now = 0.0;
  auto pump = [&](int n) {
    for (int i = 0; i < n; ++i) {
      w.set_tick(++frame);
      now += 1.0 / 60.0;
      server.tick(now);
      a.peer->tick(now);
      b.peer->tick(now);
    }
  };
  pump(3);
  KCD2S_CHECK(a.peer->owns(avatarA));   // A learned ownership
  KCD2S_CHECK(!b.peer->owns(avatarA));  // B does not own it yet

  const std::uint8_t epoch_before = w.slot(avatarA)->stateEpoch;

  // A disconnects: the server re-delegates avatarA to the remaining client B and bumps the epoch.
  server.remove_client(a.pid);
  const auto* sp = w.slot(avatarA);
  KCD2S_CHECK_EQ(sp->owner, b.pid);                                             // re-elected to B
  KCD2S_CHECK_EQ(sp->stateEpoch, static_cast<std::uint8_t>(epoch_before + 1));  // fenced

  pump(3);                             // B receives SET_OWNER
  KCD2S_CHECK(b.peer->owns(avatarA));  // B now owns it and knows the new epoch

  // B can author the owner field; server accepts it (new owner, current epoch).
  b.peer->send_input(avatarA, {{FieldId{0}, FieldValue{Vec3{200, 0, 200}}}});
  pump(4);
  KCD2S_CHECK((w.slot(avatarA)->position == Vec3{200, 0, 200}));

  // A's stale input (it still believes it owns avatarA at the old epoch) has no effect: A is no
  // longer a serviced client, and even if its bytes arrived they carry the pre-handoff epoch.
  a.peer->send_input(avatarA, {{FieldId{0}, FieldValue{Vec3{-9, -9, -9}}}});
  pump(4);
  KCD2S_CHECK((w.slot(avatarA)->position == Vec3{200, 0, 200}));  // unchanged by the departed owner
}

KCD2S_TEST(upstream, delegation_to_server_when_no_client_left) {
  SchemaRegistry reg = schema();
  Server server(reg);
  auto& w = server.world();
  const auto avatar_t = reg.get("Avatar")->id;

  w.set_tick(1);
  const EntityId avatarA = w.spawn(avatar_t);
  w.write(avatarA, 0, Vec3{0, 0, 0});

  Peer a = join(server, reg, avatarA);
  server.set_owner(avatarA, a.pid);
  const std::uint8_t epoch_before = w.slot(avatarA)->stateEpoch;

  server.remove_client(a.pid);  // last client leaves -> entity becomes server-owned
  const auto* sp = w.slot(avatarA);
  KCD2S_CHECK_EQ(sp->owner, kcd2s::core::PeerId{0});                            // server-owned
  KCD2S_CHECK_EQ(sp->stateEpoch, static_cast<std::uint8_t>(epoch_before + 1));  // still fenced
}

KCD2S_TEST(upstream, auth_server_field_never_accepted_upstream) {
  SchemaRegistry reg = schema();
  Server server(reg);
  auto& w = server.world();
  const auto avatar_t = reg.get("Avatar")->id;

  w.set_tick(1);
  const EntityId avatarA = w.spawn(avatar_t);
  w.write(avatarA, 1, 1.0f);  // health, AUTH_SERVER

  Peer a = join(server, reg, avatarA);
  server.set_owner(avatarA, a.pid);
  kcd2s::core::Tick frame = 1;
  auto pump = [&](int n) {
    for (int i = 0; i < n; ++i) {
      w.set_tick(++frame);
      server.tick(0.0);
      a.peer->tick(0.0);
    }
  };
  pump(3);

  // Even the owner cannot set an AUTH_SERVER field from upstream.
  a.peer->send_input(avatarA, {{FieldId{1}, FieldValue{0.0f}}});  // try to zero health
  pump(4);

  const auto* sp = w.slot(avatarA);
  (void)sp;
  KCD2S_CHECK_EQ(std::get<float>(*w.read(avatarA, 1)), 1.0f);  // health unchanged by upstream
}

KCD2S_TEST(upstream, native_look_owner_state_and_disable_propagate) {
  auto reg = kcd2s::local::player_schema();
  Server server(reg);
  auto avatar = kcd2s::local::spawn_player(server.world());
  Peer a = join(server, reg, avatar), b = join(server, reg, avatar);
  server.set_owner(avatar, a.pid);
  kcd2s::core::Tick frame = 0;
  auto pump = [&](int count) {
    for (int i = 0; i < count; ++i) {
      server.world().set_tick(++frame);
      server.tick(0);
      a.peer->tick(0);
      b.peer->tick(0);
    }
  };
  pump(4);
  using namespace kcd2s::local;
  a.peer->send_input(avatar, {{LookTuning, Vec3{0.3f, 0.25f, 0}},
                              {LookTarget, Vec3{123, 2040, 65}},
                              {LookEnabled, true},
                              {LookValid, true}});
  pump(5);
  auto replica = b.peer->mirror().get(avatar);
  KCD2S_CHECK(replica);
  KCD2S_CHECK((std::get<Vec3>(replica->fields[LookTarget]) == Vec3{123, 2040, 65}));
  KCD2S_CHECK(std::get<bool>(replica->fields[LookEnabled]) &&
              std::get<bool>(replica->fields[LookValid]));
  KCD2S_CHECK((std::get<Vec3>(replica->fields[LookTuning]) == Vec3{0.3f, 0.25f, 0}));
  b.peer->send_input(avatar, {{LookTarget, Vec3{0, 0, 0}}, {LookEnabled, false}});
  pump(5);
  KCD2S_CHECK((std::get<Vec3>(*server.world().read(avatar, LookTarget)) == Vec3{123, 2040, 65}));
  KCD2S_CHECK(std::get<bool>(*server.world().read(avatar, LookEnabled)));
  a.peer->send_input(avatar, {{LookTarget, Vec3{124, 2040, 50}}, {LookEnabled, false}});
  pump(5);
  replica = b.peer->mirror().get(avatar);
  KCD2S_CHECK(replica);
  KCD2S_CHECK(!std::get<bool>(replica->fields[LookEnabled]));
  KCD2S_CHECK((std::get<Vec3>(replica->fields[LookTarget]) == Vec3{124, 2040, 50}));
}

KCD2S_TEST(upstream, rider_native_completion_owner_and_detach_propagate) {
  using namespace kcd2s::local;
  auto reg = player_schema();
  Server server(reg);
  auto avatar = spawn_player(server.world());
  Peer a = join(server, reg, avatar), b = join(server, reg, avatar);
  server.set_owner(avatar, a.pid);
  kcd2s::core::Tick frame = 0;
  auto pump = [&](int count) {
    for (int i = 0; i < count; ++i) {
      server.world().set_tick(++frame);
      server.tick(0);
      a.peer->tick(0);
      b.peer->tick(0);
    }
  };
  pump(4);
  auto mounted = encode_rider({123, 8, 17, 2, 1});
  a.peer->send_input(avatar, {{NativeRiderState, mounted}});
  pump(5);
  auto replica = b.peer->mirror().get(avatar);
  KCD2S_CHECK(replica);
  auto rider = decode_rider(std::get<std::string>(replica->fields[NativeRiderState]));
  KCD2S_CHECK(rider);
  KCD2S_CHECK_EQ(rider->horse, 123u);
  KCD2S_CHECK_EQ(rider->generation, 8u);
  KCD2S_CHECK_EQ(rider->epoch, 17u);
  KCD2S_CHECK_EQ(rider->phase, 2u);
  KCD2S_CHECK_EQ(rider->active_actions, 1u);
  b.peer->send_input(avatar, {{NativeRiderState, encode_rider({999, 9, 17, 0})}});
  pump(5);
  KCD2S_CHECK_EQ(std::get<std::string>(*server.world().read(avatar, NativeRiderState)), mounted);
  // The phase can stay riding while the owner releases the action tail.
  a.peer->send_input(avatar, {{NativeRiderState, encode_rider({123, 8, 17, 2, 0})}});
  pump(5);
  replica = b.peer->mirror().get(avatar);
  rider = decode_rider(std::get<std::string>(replica->fields[NativeRiderState]));
  KCD2S_CHECK(rider);
  KCD2S_CHECK_EQ(rider->phase, 2u);
  KCD2S_CHECK_EQ(rider->active_actions, 0u);
  a.peer->send_input(avatar, {{NativeRiderState, encode_rider({123, 8, 18, 0})}});
  pump(5);
  replica = b.peer->mirror().get(avatar);
  rider = decode_rider(std::get<std::string>(replica->fields[NativeRiderState]));
  KCD2S_CHECK(rider);
  KCD2S_CHECK_EQ(rider->phase, 0u);
  KCD2S_CHECK_EQ(rider->epoch, 18u);
  KCD2S_CHECK(!decode_rider(mounted.substr(0, 21)));
  KCD2S_CHECK(!decode_rider(encode_rider({123, 8, 17, 2, 4})));
  KCD2S_CHECK(!decode_rider(encode_rider({123, 8, 17, 255})));
  KCD2S_CHECK(!decode_rider(encode_rider({0, 8, 17, 2})));
  KCD2S_CHECK(!decode_rider(encode_rider({123, 0, 17, 2})));
}

// §7 native force: the server-authoritative FORCE_XFORM teleport bridge and its authorisation
// fence.
KCD2S_TEST(upstream, native_force_transform_delivers_to_scope_and_is_accepted_by_owner) {
  SchemaRegistry reg = schema();
  Server server(reg);
  auto& w = server.world();
  const auto avatar_t = reg.get("Avatar")->id;

  w.set_tick(1);
  const EntityId avatarA = w.spawn(avatar_t);
  const EntityId avatarB = w.spawn(avatar_t);
  const EntityId entity = w.spawn(avatar_t);  // the entity both clients observe
  w.write(avatarA, 0, Vec3{0, 0, 0});
  w.write(avatarB, 0, Vec3{0, 0, 0});
  w.write(entity, 0, Vec3{0, 0, 0});

  Peer a = join(server, reg, avatarA);
  Peer b = join(server, reg, avatarB);
  server.set_owner(entity, a.pid);  // a owns the observed entity; b is a pure observer

  kcd2s::core::Tick frame = 1;
  auto pump = [&](int n) {
    for (int i = 0; i < n; ++i) {
      w.set_tick(++frame);
      server.tick(0.0);
      a.peer->tick(0.0);
      b.peer->tick(0.0);
    }
  };
  pump(4);
  KCD2S_CHECK(a.peer->owns(entity));                     // owner learned via SET_OWNER
  KCD2S_CHECK(b.peer->mirror().get(entity) != nullptr);  // observer replicated the entity

  // Authoritative teleport: accepted, slot updated, delivered to both in-scope holders.
  KCD2S_CHECK(server.force_transform(entity, Vec3{5, 6, 7}));
  pump(3);
  KCD2S_CHECK((w.slot(entity)->position == Vec3{5, 6, 7}));
  auto fa = a.peer->take_forced_transforms();
  auto fb = b.peer->take_forced_transforms();
  KCD2S_CHECK_EQ(fa.size(), std::size_t(1));
  KCD2S_CHECK_EQ(fb.size(), std::size_t(1));
  if (!fa.empty()) {
    KCD2S_CHECK(fa[0].entity == entity);
    KCD2S_CHECK((fa[0].position == Vec3{5, 6, 7}));
  }
  if (!fb.empty())
    KCD2S_CHECK(fb[0].entity == entity);
  // Draining is one-shot, like take_melee().
  KCD2S_CHECK(a.peer->take_forced_transforms().empty());

  // The owner adopted the forced epoch, so its subsequent input is still authoritative.
  a.peer->send_input(entity, {{FieldId{0}, FieldValue{Vec3{11, 0, 0}}}});
  pump(4);
  KCD2S_CHECK((w.slot(entity)->position == Vec3{11, 0, 0}));
}

KCD2S_TEST(upstream, native_force_transform_owner_input_before_drain_cannot_echo_old_pose) {
  SchemaRegistry reg = schema();
  Server server(reg);
  auto& w = server.world();
  const auto avatar_t = reg.get("Avatar")->id;

  w.set_tick(1);
  const EntityId avatarA = w.spawn(avatar_t);
  w.write(avatarA, 0, Vec3{0, 0, 0});
  Peer a = join(server, reg, avatarA);
  server.set_owner(avatarA, a.pid);

  kcd2s::core::Tick frame = 1;
  auto pump = [&](int n) {
    for (int i = 0; i < n; ++i) {
      w.set_tick(++frame);
      server.tick(0.0);
      a.peer->tick(0.0);
    }
  };
  pump(4);
  KCD2S_CHECK(a.peer->owns(avatarA));

  KCD2S_CHECK(server.force_transform(avatarA, Vec3{5, 6, 7}));
  pump(3);
  KCD2S_CHECK((w.slot(avatarA)->position == Vec3{5, 6, 7}));

  // The command has arrived but the engine has not drained it yet. Input captured in this window
  // still holds the pre-teleport pose and must not be accepted by the server.
  a.peer->send_input(avatarA, {{FieldId{0}, FieldValue{Vec3{1, 1, 1}}}});
  pump(4);
  KCD2S_CHECK((w.slot(avatarA)->position == Vec3{5, 6, 7}));

  // Draining adopts the forced epoch; input captured after the teleport is applied is accepted.
  auto forced = a.peer->take_forced_transforms();
  KCD2S_CHECK_EQ(forced.size(), std::size_t(1));
  a.peer->send_input(avatarA, {{FieldId{0}, FieldValue{Vec3{5, 6, 8}}}});
  pump(4);
  KCD2S_CHECK((w.slot(avatarA)->position == Vec3{5, 6, 8}));
}

KCD2S_TEST(upstream, native_force_transform_rejects_invalid_targets_and_coordinates) {
  SchemaRegistry reg = schema();
  Server server(reg);
  auto& w = server.world();
  const auto avatar_t = reg.get("Avatar")->id;
  w.set_tick(1);
  const EntityId avatarA = w.spawn(avatar_t);
  w.write(avatarA, 0, Vec3{1, 2, 3});

  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  // Non-finite coordinates are rejected before any mutation.
  KCD2S_CHECK(!server.force_transform(avatarA, Vec3{nan, 0, 0}));
  KCD2S_CHECK(!server.force_transform(avatarA, Vec3{0, inf, 0}));
  KCD2S_CHECK((w.slot(avatarA)->position == Vec3{1, 2, 3}));  // slot untouched

  // Unknown id is rejected.
  KCD2S_CHECK(!server.force_transform(kcd2s::core::kInvalidEntity, Vec3{0, 0, 0}));
  // Stale-reconnect fence: the reconnected avatar reuses the old storage slot but gets a new id,
  // so a command naming the departed avatar resolves to nothing and cannot mutate the new one.
  w.despawn(avatarA);
  const EntityId reconnected = w.spawn(avatar_t);
  KCD2S_CHECK(reconnected != avatarA);
  w.write(reconnected, 0, Vec3{4, 5, 6});
  KCD2S_CHECK(!server.force_transform(avatarA, Vec3{9, 9, 9}));
  KCD2S_CHECK((w.slot(reconnected)->position == Vec3{4, 5, 6}));
}

KCD2S_TEST(upstream, native_force_transform_wire_rejects_malformed_and_nonfinite) {
  const EntityId e = 70000;  // above 16 bits, so the full 32-bit id must survive the roundtrip
  auto good = encode_force_transform(e, 7, Vec3{1.5f, -2.0f, 3.0f});
  EntityId de = 0;
  std::uint8_t dep = 0;
  Vec3 dp{};
  KCD2S_CHECK(decode_force_transform(good, de, dep, dp));
  KCD2S_CHECK(de == e);
  KCD2S_CHECK_EQ(dep, std::uint8_t(7));
  KCD2S_CHECK((dp == Vec3{1.5f, -2.0f, 3.0f}));

  // Truncated payload -> overrun -> rejected.
  auto truncated = good;
  truncated.resize(good.size() - 2);
  KCD2S_CHECK(!decode_force_transform(truncated, de, dep, dp));

  // A different control kind is not a FORCE_XFORM.
  KCD2S_CHECK(!decode_force_transform(encode_force_state(e, 7), de, dep, dp));

  // Non-finite coordinates on the wire are rejected at decode.
  const float nan = std::numeric_limits<float>::quiet_NaN();
  KCD2S_CHECK(!decode_force_transform(encode_force_transform(e, 7, Vec3{nan, 0, 0}), de, dep, dp));

  // A varuint id that does not fit in the 32-bit EntityId is malformed, not silently truncated.
  kcd2s::core::BitWriter w;
  w.write_bits(5, 8);  // kCtrlForceTransform
  w.write_varuint(0x1'0000'0000ull);
  w.write_bits(0, 8);
  for (int i = 0; i < 3; ++i)
    w.write_bits(0, 32);
  KCD2S_CHECK(!decode_force_transform(w.bytes(), de, dep, dp));
}

// A single owner + observer sharing one teleported entity, reused by the ordering and boundedness
// tests below.
namespace {
struct ForcePair {
  SchemaRegistry reg = schema();
  Server server{reg};
  EntityId avatarA = 0, avatarB = 0, entity = 0;
  std::unique_ptr<Peer> a, b;
  kcd2s::core::Tick frame = 1;
  ForcePair() {
    auto& w = server.world();
    const auto t = reg.get("Avatar")->id;
    w.set_tick(1);
    avatarA = w.spawn(t);
    avatarB = w.spawn(t);
    entity = w.spawn(t);
    w.write(avatarA, 0, Vec3{0, 0, 0});
    w.write(avatarB, 0, Vec3{0, 0, 0});
    w.write(entity, 0, Vec3{0, 0, 0});
    a = std::make_unique<Peer>(join(server, reg, avatarA));
    b = std::make_unique<Peer>(join(server, reg, avatarB));
    server.set_owner(entity, a->pid);
    pump(4);
  }
  void pump(int n) {
    for (int i = 0; i < n; ++i) {
      server.world().set_tick(++frame);
      server.tick(0.0);
      a->peer->tick(0.0);
      b->peer->tick(0.0);
    }
  }
};
}  // namespace

// Ordering across the uint8 epoch wrap: the server bumps the epoch once per force, so after >255
// teleports it rolls 255 -> 0. A raw `epoch >= known` fence would reject every post-wrap teleport
// (e.g. 0 >= 255 is false); the signed-difference fence keeps accepting them in order.
KCD2S_TEST(upstream, native_force_transform_ordering_survives_epoch_wrap) {
  ForcePair f;
  KCD2S_CHECK(f.b->peer->mirror().get(f.entity) != nullptr);
  for (int i = 0; i < 254; ++i) {
    KCD2S_CHECK(f.server.force_transform(f.entity, Vec3{static_cast<float>(i), 0, 0}));
    f.pump(1);
  }
  f.a->peer->take_forced_transforms();
  f.b->peer->take_forced_transforms();
  const std::uint8_t pre = f.server.world().slot(f.entity)->stateEpoch;  // near the wrap point

  // Cross the 255 -> 0 boundary.
  Vec3 last{0, 0, 0};
  for (int i = 0; i < 6; ++i) {
    last = Vec3{1000.0f + static_cast<float>(i), 0, 0};
    KCD2S_CHECK(f.server.force_transform(f.entity, last));
    f.pump(1);
  }
  KCD2S_CHECK(f.server.world().slot(f.entity)->stateEpoch < pre);  // epoch has wrapped
  auto fb = f.b->peer->take_forced_transforms();
  KCD2S_CHECK(!fb.empty());                   // post-wrap teleports were NOT dropped
  KCD2S_CHECK((fb.back().position == last));  // and arrived in order
  KCD2S_CHECK((f.server.world().slot(f.entity)->position == last));
}

// Boundedness: a client that never drains must not let the pending queue grow without limit. The
// oldest pending teleport is dropped so the queue stays capped while keeping the newest.
KCD2S_TEST(upstream, native_force_transform_queue_is_bounded) {
  ForcePair f;
  Vec3 last{0, 0, 0};
  for (int i = 0; i < 300; ++i) {  // > the 256 cap, never draining the observer
    last = Vec3{static_cast<float>(i), 0, 0};
    KCD2S_CHECK(f.server.force_transform(f.entity, last));
    f.pump(1);
  }
  auto fb = f.b->peer->take_forced_transforms();
  KCD2S_CHECK(fb.size() <= std::size_t(256));  // bounded
  KCD2S_CHECK(!fb.empty());
  KCD2S_CHECK((fb.back().position == last));  // newest retained
}

// Concern (1): the teleport must update the authoritative replicated position FIELD, so subsequent
// snapshots and SDK reads report it -- not merely the interest-hot slot.position.
KCD2S_TEST(upstream, native_force_transform_updates_replicated_position_field) {
  ForcePair f;
  KCD2S_CHECK(f.b->peer->mirror().get(f.entity) != nullptr);
  KCD2S_CHECK(f.server.force_transform(f.entity, Vec3{123, 45, 67}));
  f.pump(4);
  const auto* read = f.server.world().read(f.entity, FieldId{0});  // position field
  KCD2S_CHECK(read != nullptr);
  if (read)
    KCD2S_CHECK((std::get<Vec3>(*read) == Vec3{123, 45, 67}));
  const auto* bmir = f.b->peer->mirror().get(f.entity);  // reached the observer via a snapshot
  KCD2S_CHECK(bmir != nullptr);
  if (bmir)
    KCD2S_CHECK((std::get<Vec3>(bmir->fields[0]) == Vec3{123, 45, 67}));
}

// The owner must receive the teleport for an entity it controls even when that entity is far
// outside its interest range. Owned entities in the owner's dimension and visibility scope
// stay in its snapshot regardless of range, so the owner also holds the entity.
KCD2S_TEST(upstream, native_force_transform_reaches_owner_beyond_its_interest_range) {
  SchemaRegistry reg = schema();
  Server server(reg);
  auto& w = server.world();
  const auto t = reg.get("Avatar")->id;
  w.set_tick(1);
  const EntityId avatarA = w.spawn(t);
  const EntityId entity = w.spawn(t);
  w.write(avatarA, 0, Vec3{0, 0, 0});
  w.write(entity, 0, Vec3{10000, 0, 0});  // far from the owner's viewer

  auto pr = make_sim_pair({}, {});
  auto slink = std::move(pr.first);
  auto clink = std::move(pr.second);
  const PeerId pid = server.add_client(slink.get(), avatarA, 5.0f);  // small interest range
  auto peer = std::make_unique<ClientPeer>(reg, clink.get());
  server.set_owner(entity, pid);
  kcd2s::core::Tick frame = 1;
  auto pump = [&](int n) {
    for (int i = 0; i < n; ++i) {
      w.set_tick(++frame);
      server.tick(0.0);
      peer->tick(0.0);
    }
  };
  pump(4);
  KCD2S_CHECK(peer->owns(entity));
  KCD2S_CHECK(peer->mirror().get(entity) != nullptr);  // owned: in scope beyond the range
  KCD2S_CHECK(server.force_transform(entity, Vec3{10001, 2, 3}));
  pump(3);
  auto fa = peer->take_forced_transforms();
  KCD2S_CHECK_EQ(fa.size(), std::size_t(1));
  if (!fa.empty())
    KCD2S_CHECK((fa[0].position == Vec3{10001, 2, 3}));
}

// Concern (2) drain validation: a command accepted while the entity was alive must be dropped at
// drain if the entity has since despawned, so the engine consumer never mutates a dead/recycled id.
KCD2S_TEST(upstream, native_force_transform_drops_command_after_despawn) {
  ForcePair f;
  KCD2S_CHECK(f.b->peer->mirror().get(f.entity) != nullptr);
  KCD2S_CHECK(f.server.force_transform(f.entity, Vec3{7, 8, 9}));
  f.pump(2);  // observer queued the teleport while the entity was alive
  f.server.world().despawn(f.entity);
  f.pump(4);  // DESTRUCT reaches the observer; the mirror drops the entity
  KCD2S_CHECK(f.b->peer->mirror().get(f.entity) == nullptr);
  auto fb = f.b->peer->take_forced_transforms();
  KCD2S_CHECK(fb.empty());  // stale command filtered at drain
}

// Concern (2): a queued command superseded by a newer forced epoch is dropped at drain; only the
// command at the currently adopted epoch survives (latest-wins).
KCD2S_TEST(upstream, native_force_transform_drops_superseded_epoch_at_drain) {
  ForcePair f;
  KCD2S_CHECK(f.server.force_transform(f.entity, Vec3{1, 1, 1}));
  f.pump(2);  // queued at epoch N
  KCD2S_CHECK(f.server.force_transform(f.entity, Vec3{2, 2, 2}));
  f.pump(2);  // queued at epoch N+1, which the client now adopts
  auto fb = f.b->peer->take_forced_transforms();
  KCD2S_CHECK_EQ(fb.size(), std::size_t(1));  // the superseded epoch N command is gone
  if (!fb.empty())
    KCD2S_CHECK((fb[0].position == Vec3{2, 2, 2}));
}

// Concern (2): a world-revision change (reconnect / map change) resets queued teleports, so a
// command accepted under the old revision never applies in the new world.
KCD2S_TEST(upstream, native_force_transform_reset_on_world_revision_change) {
  ForcePair f;
  KCD2S_CHECK(f.server.configure_world(f.b->pid, WorldConfig{1u, "trosecko"}));
  f.pump(3);
  KCD2S_CHECK(f.server.force_transform(f.entity, Vec3{3, 3, 3}));
  f.pump(2);  // queued under world revision 1
  KCD2S_CHECK(f.server.configure_world(f.b->pid, WorldConfig{2u, "kutnohorsko"}));
  f.pump(3);  // observer adopts the new revision and resets queued teleports
  auto fb = f.b->peer->take_forced_transforms();
  KCD2S_CHECK(fb.empty());
}

// Avatar with a replicated rotation, for FORCE_ROT (the plain schema above has none).
static SchemaRegistry rotation_schema() {
  SchemaRegistry reg;
  reg.define("Avatar")
      .field("position", FieldKind::Vec3, Priority::High, Authority::Owner)   // field 0
      .field("health", FieldKind::F32, Priority::Med, Authority::Server)      // field 1
      .field("rotation", FieldKind::Quat, Priority::High, Authority::Owner);  // field 2
  return reg;
}

KCD2S_TEST(upstream, native_force_rotation_merges_with_position_and_fences_owner_input) {
  SchemaRegistry reg = rotation_schema();
  Server server(reg);
  auto& w = server.world();
  const auto avatar_t = reg.get("Avatar")->id;
  w.set_tick(1);
  const EntityId avatarA = w.spawn(avatar_t);
  w.write(avatarA, 0, Vec3{0, 0, 0});
  Peer a = join(server, reg, avatarA);
  server.set_owner(avatarA, a.pid);
  kcd2s::core::Tick frame = 1;
  auto pump = [&](int n) {
    for (int i = 0; i < n; ++i) {
      w.set_tick(++frame);
      server.tick(0.0);
      a.peer->tick(0.0);
    }
  };
  pump(4);
  KCD2S_CHECK(a.peer->owns(avatarA));

  // A position and a rotation forced back to back both reach the engine in one drained command.
  const Quat turned{0.0f, 0.0f, 0.70710678f, 0.70710678f};
  KCD2S_CHECK(server.force_transform(avatarA, Vec3{5, 6, 7}));
  KCD2S_CHECK(server.force_rotation(avatarA, turned));
  pump(3);
  KCD2S_CHECK((std::get<Quat>(*w.read(avatarA, 2)) == turned));

  // Owner input captured before the drain still carries the old rotation and is rejected.
  a.peer->send_input(avatarA, {{FieldId{2}, FieldValue{Quat{}}}});
  pump(4);
  KCD2S_CHECK((std::get<Quat>(*w.read(avatarA, 2)) == turned));

  auto forced = a.peer->take_forced_transforms();
  KCD2S_CHECK_EQ(forced.size(), std::size_t(1));
  if (!forced.empty()) {
    KCD2S_CHECK(forced[0].has_position);
    KCD2S_CHECK(forced[0].has_rotation);
    KCD2S_CHECK((forced[0].position == Vec3{5, 6, 7}));
    KCD2S_CHECK((forced[0].rotation == turned));
  }

  // After the drain the owner's input is authoritative again.
  const Quat later{0.0f, 0.0f, 0.0f, 1.0f};
  a.peer->send_input(avatarA, {{FieldId{2}, FieldValue{later}}});
  pump(4);
  KCD2S_CHECK((std::get<Quat>(*w.read(avatarA, 2)) == later));
}

KCD2S_TEST(upstream, native_force_rotation_rejects_invalid_rotations_and_types) {
  SchemaRegistry reg = rotation_schema();
  Server server(reg);
  auto& w = server.world();
  w.set_tick(1);
  const EntityId avatarA = w.spawn(reg.get("Avatar")->id);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  KCD2S_CHECK(!server.force_rotation(avatarA, Quat{nan, 0, 0, 1}));
  KCD2S_CHECK(!server.force_rotation(avatarA, Quat{0, 0, 0, 0}));
  KCD2S_CHECK(!server.force_rotation(avatarA, Quat{0, 0, 0, 2}));
  KCD2S_CHECK(!server.force_rotation(kcd2s::core::kInvalidEntity, Quat{}));
  KCD2S_CHECK((std::get<Quat>(*w.read(avatarA, 2)) == Quat{}));

  // A type without a replicated Quat "rotation" field cannot be rotated.
  SchemaRegistry plain = schema();
  Server plain_server(plain);
  plain_server.world().set_tick(1);
  const EntityId flat = plain_server.world().spawn(plain.get("Avatar")->id);
  KCD2S_CHECK(!plain_server.force_rotation(flat, Quat{}));
}

KCD2S_TEST(upstream, native_force_rotation_wire_rejects_malformed_and_non_unit) {
  const EntityId e = 70000;
  const Quat turned{0.0f, 0.0f, 0.70710678f, 0.70710678f};
  auto good = encode_force_rotation(e, 9, turned);
  EntityId de = 0;
  std::uint8_t dep = 0;
  Quat dq{};
  KCD2S_CHECK(decode_force_rotation(good, de, dep, dq));
  KCD2S_CHECK(de == e);
  KCD2S_CHECK_EQ(dep, std::uint8_t(9));
  KCD2S_CHECK((dq == turned));
  auto truncated = good;
  truncated.resize(good.size() - 2);
  KCD2S_CHECK(!decode_force_rotation(truncated, de, dep, dq));
  KCD2S_CHECK(!decode_force_rotation(encode_force_transform(e, 9, Vec3{}), de, dep, dq));
  KCD2S_CHECK(!decode_force_rotation(encode_force_rotation(e, 9, Quat{0, 0, 0, 3}), de, dep, dq));
}
