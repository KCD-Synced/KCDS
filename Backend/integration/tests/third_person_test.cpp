#include "kcd2s/integration/server.hpp"
#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/third_person.hpp"
#include "kcd2s/testkit.hpp"
using namespace kcd2s;
using namespace kcd2s::integration;

// The server's third-person switch: THIRD_PERSON reaches the player's own client,
// which hands each change over once.
KCD2S_TEST(third_person, reaches_the_player_client_once_per_order) {
  replication::SchemaRegistry schema;
  auto type = schema.define("Player")
                  .field("position", replication::FieldKind::Vec3, replication::Priority::High,
                         replication::Authority::Owner)
                  .id();
  Server server(schema);
  server.world().set_tick(1);
  auto e = server.world().spawn(type);
  server.world().write(e, 0, core::Vec3{});
  auto links = transport::make_loopback_pair();
  auto p = server.add_client(links.first.get(), e, 10000);
  WorldConfig c;
  KCD2S_CHECK(server.configure_world(p, c));
  server.set_owner(e, p);
  ClientPeer client(schema, links.second.get());
  double now = 0;
  auto tick = [&] {
    now += 1.0 / 60;
    server.world().set_tick(server.world().tick() + 1);
    server.tick(now);
    client.tick(now);
  };
  for (int i = 0; i < 8; ++i) {
    tick();
  }
  KCD2S_CHECK(!client.take_third_person().has_value());

  KCD2S_CHECK(server.send_third_person(e, kThirdPersonForced));
  KCD2S_CHECK(!server.send_third_person(e + 1, kThirdPersonForced));
  KCD2S_CHECK(!server.send_third_person(e, 3));
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  auto mode = client.take_third_person();
  KCD2S_CHECK(mode.has_value() && *mode == kThirdPersonForced);
  KCD2S_CHECK(!client.take_third_person().has_value());

  KCD2S_CHECK(server.send_third_person(e, kThirdPersonDisabled));
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  mode = client.take_third_person();
  KCD2S_CHECK(mode.has_value() && *mode == kThirdPersonDisabled);

  KCD2S_CHECK(server.send_third_person(e, kThirdPersonAllowed));
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  mode = client.take_third_person();
  KCD2S_CHECK(mode.has_value() && *mode == kThirdPersonAllowed);
}

// Handcuffs travel the same way: CUFFS reaches the player's own client once.
KCD2S_TEST(cuffs, reach_the_player_client_once_per_order) {
  replication::SchemaRegistry schema;
  auto type = schema.define("Player")
                  .field("position", replication::FieldKind::Vec3, replication::Priority::High,
                         replication::Authority::Owner)
                  .id();
  Server server(schema);
  server.world().set_tick(1);
  auto e = server.world().spawn(type);
  server.world().write(e, 0, core::Vec3{});
  auto links = transport::make_loopback_pair();
  auto p = server.add_client(links.first.get(), e, 10000);
  KCD2S_CHECK(server.configure_world(p, WorldConfig{}));
  server.set_owner(e, p);
  ClientPeer client(schema, links.second.get());
  double now = 0;
  auto tick = [&] {
    now += 1.0 / 60;
    server.world().set_tick(server.world().tick() + 1);
    server.tick(now);
    client.tick(now);
  };
  for (int i = 0; i < 8; ++i) {
    tick();
  }
  KCD2S_CHECK(!client.take_cuffs().has_value());
  KCD2S_CHECK(server.send_cuffs(e, true));
  KCD2S_CHECK(!server.send_cuffs(e + 1, true));
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  auto cuffed = client.take_cuffs();
  KCD2S_CHECK(cuffed.has_value() && *cuffed);
  KCD2S_CHECK(!client.take_cuffs().has_value());
  KCD2S_CHECK(server.send_cuffs(e, false));
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  cuffed = client.take_cuffs();
  KCD2S_CHECK(cuffed.has_value() && !*cuffed);

  bool value = false;
  KCD2S_CHECK(decode_cuffs(encode_cuffs(true), value) && value);
  KCD2S_CHECK(decode_cuffs(encode_cuffs(false), value) && !value);
  KCD2S_CHECK(!decode_cuffs({kCuffsTag, 2}, value));
  KCD2S_CHECK(!decode_cuffs({kCuffsTag}, value));
  KCD2S_CHECK(!decode_cuffs({kCuffsTag, 1, 0}, value));
}

KCD2S_TEST(third_person, rejects_malformed_messages) {
  std::uint8_t mode = 9;
  for (std::uint8_t m : {kThirdPersonAllowed, kThirdPersonDisabled, kThirdPersonForced}) {
    KCD2S_CHECK(decode_third_person(encode_third_person(m), mode));
    KCD2S_CHECK(mode == m);
  }
  KCD2S_CHECK(!decode_third_person({kThirdPersonTag, 3}, mode));
  KCD2S_CHECK(!decode_third_person({kThirdPersonTag}, mode));
  KCD2S_CHECK(!decode_third_person({kThirdPersonTag, 1, 0}, mode));
  KCD2S_CHECK(!decode_third_person({std::uint8_t(kThirdPersonTag + 1), 1}, mode));
}

// The game's own interactions a server allows (INTERACTION_RULES) reach the player's own client
// once per change, and a malformed message is refused.
KCD2S_TEST(interaction_rules, reach_the_player_client_once_per_change) {
  replication::SchemaRegistry schema;
  auto type = schema.define("Player")
                  .field("position", replication::FieldKind::Vec3, replication::Priority::High,
                         replication::Authority::Owner)
                  .id();
  Server server(schema);
  server.world().set_tick(1);
  auto e = server.world().spawn(type);
  server.world().write(e, 0, core::Vec3{});
  auto links = transport::make_loopback_pair();
  auto p = server.add_client(links.first.get(), e, 10000);
  KCD2S_CHECK(server.configure_world(p, WorldConfig{}));
  server.set_owner(e, p);
  ClientPeer client(schema, links.second.get());
  double now = 0;
  auto tick = [&] {
    now += 1.0 / 60;
    server.world().set_tick(server.world().tick() + 1);
    server.tick(now);
    client.tick(now);
  };
  for (int i = 0; i < 8; ++i) {
    tick();
  }
  KCD2S_CHECK(!client.take_interaction_rules().has_value());
  KCD2S_CHECK(server.send_interaction_rules(e, kAllowHorseWhistle | kAllowNpcTalk));
  KCD2S_CHECK(!server.send_interaction_rules(e + 1, kAllowHorseWhistle));
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  auto allowed = client.take_interaction_rules();
  KCD2S_CHECK(allowed.has_value() && *allowed == (kAllowHorseWhistle | kAllowNpcTalk));
  KCD2S_CHECK(!client.take_interaction_rules().has_value());

  std::uint16_t value = 0;
  KCD2S_CHECK(decode_interaction_rules(encode_interaction_rules(kAllowedBits), value));
  KCD2S_CHECK(value == kAllowedBits);
  KCD2S_CHECK(
      decode_interaction_rules({kInteractionRulesTag, std::uint8_t(kRefuseNpcChatter), 0}, value));
  KCD2S_CHECK(value == kRefuseNpcChatter);
  KCD2S_CHECK(
      decode_interaction_rules({kInteractionRulesTag, std::uint8_t(kRefusePointing), 0}, value));
  KCD2S_CHECK(value == kRefusePointing);
  KCD2S_CHECK(!decode_interaction_rules({kInteractionRulesTag, 0, 2}, value));
  KCD2S_CHECK(!decode_interaction_rules({kInteractionRulesTag}, value));
  KCD2S_CHECK(!decode_interaction_rules({kInteractionRulesTag, 1, 0, 0}, value));
}

// One player attached to another: the order survives the wire, and only the right
// message decodes.
KCD2S_TEST(player_attach, encodes_and_decodes) {
  PlayerAttach attach;
  attach.flags = kAttachIsLocalPlayer;
  attach.player = 3;
  attach.target = 7;
  attach.right = 0.5f;
  attach.forward = -0.25f;
  attach.up = 0.1f;
  attach.yaw = 90.0f;
  PlayerAttach read;
  KCD2S_CHECK(decode_player_attach(encode_player_attach(attach), read));
  KCD2S_CHECK(read.flags == kAttachIsLocalPlayer && read.player == 3);
  KCD2S_CHECK(read.target == 7 && read.right == 0.5f && read.forward == -0.25f);
  KCD2S_CHECK(read.up == 0.1f && read.yaw == 90.0f);
  KCD2S_CHECK(!decode_player_attach({kPlayerAttachTag, 1}, read));
  KCD2S_CHECK(!decode_player_attach(encode_frozen(true), read));
}

// A frozen player: FROZEN reaches the player's own client once per order.
KCD2S_TEST(frozen, reaches_the_player_client_once_per_order) {
  replication::SchemaRegistry schema;
  auto type = schema.define("Player")
                  .field("position", replication::FieldKind::Vec3, replication::Priority::High,
                         replication::Authority::Owner)
                  .id();
  Server server(schema);
  server.world().set_tick(1);
  auto e = server.world().spawn(type);
  server.world().write(e, 0, core::Vec3{});
  auto links = transport::make_loopback_pair();
  auto p = server.add_client(links.first.get(), e, 10000);
  KCD2S_CHECK(server.configure_world(p, WorldConfig{}));
  server.set_owner(e, p);
  ClientPeer client(schema, links.second.get());
  double now = 0;
  auto tick = [&] {
    now += 1.0 / 60;
    server.world().set_tick(server.world().tick() + 1);
    server.tick(now);
    client.tick(now);
  };
  for (int i = 0; i < 8; ++i) {
    tick();
  }
  KCD2S_CHECK(!client.take_frozen().has_value());
  KCD2S_CHECK(server.send_frozen(e, true));
  KCD2S_CHECK(!server.send_frozen(e + 1, true));
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  auto frozen = client.take_frozen();
  KCD2S_CHECK(frozen.has_value() && *frozen);
  KCD2S_CHECK(!client.take_frozen().has_value());

  bool value = false;
  KCD2S_CHECK(decode_frozen(encode_frozen(false), value) && !value);
  KCD2S_CHECK(!decode_frozen({kFrozenTag, 2}, value));
  KCD2S_CHECK(!decode_frozen({kCuffsTag, 1}, value));
}

// NoClip allowed per player: NOCLIP_ALLOWED reaches the player's own client once per
// change.
KCD2S_TEST(noclip_allowed, reaches_the_player_client_once_per_change) {
  replication::SchemaRegistry schema;
  auto type = schema.define("Player")
                  .field("position", replication::FieldKind::Vec3, replication::Priority::High,
                         replication::Authority::Owner)
                  .id();
  Server server(schema);
  server.world().set_tick(1);
  auto e = server.world().spawn(type);
  server.world().write(e, 0, core::Vec3{});
  auto links = transport::make_loopback_pair();
  auto p = server.add_client(links.first.get(), e, 10000);
  KCD2S_CHECK(server.configure_world(p, WorldConfig{}));
  server.set_owner(e, p);
  ClientPeer client(schema, links.second.get());
  double now = 0;
  auto tick = [&] {
    now += 1.0 / 60;
    server.world().set_tick(server.world().tick() + 1);
    server.tick(now);
    client.tick(now);
  };
  for (int i = 0; i < 8; ++i) {
    tick();
  }
  KCD2S_CHECK(!client.take_noclip_allowed().has_value());
  KCD2S_CHECK(server.send_noclip_allowed(e, false));
  KCD2S_CHECK(!server.send_noclip_allowed(e + 1, false));
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  auto allowed = client.take_noclip_allowed();
  KCD2S_CHECK(allowed.has_value() && !*allowed);
  KCD2S_CHECK(!client.take_noclip_allowed().has_value());

  bool value = false;
  KCD2S_CHECK(decode_noclip_allowed(encode_noclip_allowed(true), value) && value);
  KCD2S_CHECK(!decode_noclip_allowed({kNoClipAllowedTag, 2}, value));
  KCD2S_CHECK(!decode_noclip_allowed({kFrozenTag, 1}, value));
}
