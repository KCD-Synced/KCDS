#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/level_edits.hpp"
#include "kcd2s/integration/server.hpp"
#include "kcd2s/testkit.hpp"
using namespace kcd2s;
using namespace kcd2s::integration;

namespace {

struct Link {
  replication::SchemaRegistry schema;
  Server server;
  std::pair<std::unique_ptr<transport::SimLink>, std::unique_ptr<transport::SimLink>> links;
  core::EntityId player = 0;
  core::PeerId peer = 0;
  ClientPeer client;
  double now = 0;

  static replication::SchemaRegistry make_schema() {
    replication::SchemaRegistry schema;
    schema.define("Player").field("position", replication::FieldKind::Vec3,
                                  replication::Priority::High, replication::Authority::Owner);
    return schema;
  }

  Link()
      : schema(make_schema()),
        server(schema),
        links(transport::make_loopback_pair()),
        client(schema, links.second.get()) {
    server.world().set_tick(1);
    player = server.world().spawn(core::type_id("Player"));
    server.world().write(player, 0, core::Vec3{});
    peer = server.add_client(links.first.get(), player, 10000);
    WorldConfig config;
    server.configure_world(peer, config);
    server.set_owner(player, peer);
    run(8);
  }

  void run(int ticks) {
    for (int i = 0; i < ticks; ++i) {
      now += 1.0 / 60;
      server.world().set_tick(server.world().tick() + 1);
      server.tick(now);
      client.tick(now);
    }
  }
};

LevelEdit brush_edit() {
  LevelEdit edit;
  edit.id = 41;
  edit.kind = LevelEditKind::Brush;
  edit.name = "objects/buildings/houses/house_a.cgf";
  edit.at[0] = 1234.5f;
  edit.at[1] = -77.25f;
  edit.at[2] = 31.0f;
  edit.hidden = true;
  return edit;
}

LevelEdit entity_edit() {
  LevelEdit edit;
  edit.id = 42;
  edit.kind = LevelEditKind::Entity;
  edit.guid = 0x0123456789abcdefULL;
  edit.name = "AnimDoor1";
  edit.at[0] = 10.0f;
  edit.has_position = true;
  edit.position[0] = 11.0f;
  edit.position[1] = 12.0f;
  edit.position[2] = 13.0f;
  edit.has_rotation = true;
  edit.rotation[2] = 0.70710677f;
  edit.rotation[3] = 0.70710677f;
  edit.has_scale = true;
  edit.scale[0] = 2.0f;
  edit.scale[1] = 2.0f;
  edit.scale[2] = 0.5f;
  return edit;
}

}  // namespace

KCD2S_TEST(level_edits, edits_round_trip_through_the_codec) {
  LevelEdit decoded;
  KCD2S_CHECK(decode_level_edit(encode_level_edit(brush_edit()), decoded));
  KCD2S_CHECK(decoded == brush_edit());
  KCD2S_CHECK(decode_level_edit(encode_level_edit(entity_edit()), decoded));
  KCD2S_CHECK(decoded == entity_edit());

  std::uint32_t id = 0;
  KCD2S_CHECK(decode_level_edit_remove(encode_level_edit_remove(41), id));
  KCD2S_CHECK_EQ(id, 41u);
  KCD2S_CHECK(encode_level_edit_remove(0).empty());

  // What cannot travel: no id, a brush without its mesh or with a guid, an entity without its
  // guid, a place out of the world, a scale out of range, a name too long.
  LevelEdit bad = brush_edit();
  bad.id = 0;
  KCD2S_CHECK(encode_level_edit(bad).empty());
  bad = brush_edit();
  bad.name.clear();
  KCD2S_CHECK(encode_level_edit(bad).empty());
  bad = brush_edit();
  bad.guid = 7;
  KCD2S_CHECK(encode_level_edit(bad).empty());
  bad = entity_edit();
  bad.guid = 0;
  KCD2S_CHECK(encode_level_edit(bad).empty());
  bad = brush_edit();
  bad.at[1] = 2e6f;
  KCD2S_CHECK(encode_level_edit(bad).empty());
  bad = entity_edit();
  bad.scale[1] = 1000.0f;
  KCD2S_CHECK(encode_level_edit(bad).empty());
  bad = brush_edit();
  bad.name = std::string(kMaxLevelEditNameLength + 1, 'a');
  KCD2S_CHECK(encode_level_edit(bad).empty());

  // A message cut short or with bytes after it is refused.
  auto bytes = encode_level_edit(entity_edit());
  auto cut = bytes;
  cut.pop_back();
  KCD2S_CHECK(!decode_level_edit(cut, decoded));
  auto longer = bytes;
  longer.push_back(0);
  KCD2S_CHECK(!decode_level_edit(longer, decoded));
  KCD2S_CHECK(!decode_level_edit_remove({kLevelEditRemoveTag}, id));
}

KCD2S_TEST(level_edits, guid_text_round_trips) {
  std::uint64_t guid = 0;
  KCD2S_CHECK(parse_level_edit_guid("0123456789abcdef", guid));
  KCD2S_CHECK_EQ(guid, 0xcdef89ab01234567ULL);
  KCD2S_CHECK(level_edit_guid_text(guid) == "0123456789abcdef");
  // As the level files write it; the key as the game's entity has it (gate_nebakov_left_suchdol1).
  KCD2S_CHECK(parse_level_edit_guid("10cbd99b-495b-4d4d", guid));
  KCD2S_CHECK_EQ(guid, 0x4d4d495b10cbd99bULL);
  KCD2S_CHECK(!parse_level_edit_guid("0123456789abcde", guid));
  KCD2S_CHECK(!parse_level_edit_guid("0123456789abcdeg", guid));
}

KCD2S_TEST(level_edits, the_server_tells_the_players_of_the_edit_world) {
  Link link;
  // Nothing reaches a client before its world is ready.
  KCD2S_CHECK(!link.server.send_level_edit(0, brush_edit(), replication::kGlobalWorld));
  KCD2S_CHECK(link.client.mark_world_ready(1));
  link.run(5);
  KCD2S_CHECK(link.server.send_level_edit(link.player, brush_edit(), replication::kGlobalWorld));
  KCD2S_CHECK(link.server.send_level_edit_remove(0, 41));
  link.run(8);
  auto changes = link.client.take_level_edit_changes();
  KCD2S_CHECK_EQ(changes.size(), std::size_t(2));
  KCD2S_CHECK(!changes[0].removed);
  KCD2S_CHECK(changes[0].edit == brush_edit());
  KCD2S_CHECK(changes[1].removed);
  KCD2S_CHECK_EQ(changes[1].edit.id, 41u);

  // A player in world 3 hears the edits of world 3 and of the global world, not of world 4.
  link.server.set_dimension(link.player, 3);
  KCD2S_CHECK(link.server.send_level_edit(0, entity_edit(), 3));
  KCD2S_CHECK(!link.server.send_level_edit(0, entity_edit(), 4));
  KCD2S_CHECK(!link.server.send_level_edit(link.player + 1, entity_edit(), 3));
  link.run(8);
  changes = link.client.take_level_edit_changes();
  KCD2S_CHECK_EQ(changes.size(), std::size_t(1));
  KCD2S_CHECK(changes[0].edit == entity_edit());
}
