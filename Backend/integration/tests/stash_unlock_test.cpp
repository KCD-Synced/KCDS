#include "kcd2s/integration/server.hpp"
#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/stash.hpp"
#include "kcd2s/testkit.hpp"
using namespace kcd2s;
using namespace kcd2s::integration;

// STASH_UNLOCK: a container's lock the player opened in their game.
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

StashUnlock unlock_of(std::uint32_t stash, StashUnlockMethod method) {
  StashUnlock unlock;
  unlock.stash = stash;
  unlock.method = method;
  return unlock;
}

}  // namespace

KCD2S_TEST(stash_unlock, round_trips_through_the_codec) {
  const StashUnlock picked = unlock_of(300, StashUnlockMethod::Pick);
  const StashUnlock keyed = unlock_of(0xFFFFFFFFu, StashUnlockMethod::Key);
  StashUnlock heard;
  const auto bytes = encode_stash_unlock(picked);
  KCD2S_CHECK(!bytes.empty() && bytes[0] == kStashUnlockTag);
  KCD2S_CHECK(decode_stash_unlock(bytes, heard) && heard == picked);
  KCD2S_CHECK(decode_stash_unlock(encode_stash_unlock(keyed), heard) && heard == keyed);
  KCD2S_CHECK(std::string(stash_unlock_method_name(StashUnlockMethod::Pick)) == "pick");
  KCD2S_CHECK(std::string(stash_unlock_method_name(StashUnlockMethod::Key)) == "key");
  // No container, an unknown method, a cut or padded message, another tag.
  KCD2S_CHECK(encode_stash_unlock(unlock_of(0, StashUnlockMethod::Pick)).empty());
  KCD2S_CHECK(encode_stash_unlock(unlock_of(5, StashUnlockMethod(3))).empty());
  auto wrong = bytes;
  wrong.back() = 3;
  KCD2S_CHECK(!decode_stash_unlock(wrong, heard));
  auto cut = bytes;
  cut.pop_back();
  KCD2S_CHECK(!decode_stash_unlock(cut, heard));
  auto padded = bytes;
  padded.push_back(0);
  KCD2S_CHECK(!decode_stash_unlock(padded, heard));
  auto other = bytes;
  other[0] = kStashShowTag;
  KCD2S_CHECK(!decode_stash_unlock(other, heard));
}

KCD2S_TEST(stash_unlock, reaches_the_server_with_the_open_after_it) {
  Link link;
  // No world yet on the client: nothing travels.
  KCD2S_CHECK(!link.client.send_stash_unlock(unlock_of(9, StashUnlockMethod::Key)));
  KCD2S_CHECK(link.client.mark_world_ready(1));
  link.run(5);
  KCD2S_CHECK(link.client.send_stash_unlock(unlock_of(9, StashUnlockMethod::Key)));
  InventoryReport open;
  open.kind = InventoryReport::Kind::StashOpen;
  open.request = 9;
  KCD2S_CHECK(link.client.send_inventory_report(open));
  KCD2S_CHECK(!link.client.send_stash_unlock(unlock_of(0, StashUnlockMethod::Pick)));
  link.run(5);
  const auto unlocks = link.server.take_stash_unlocks();
  KCD2S_CHECK_EQ(unlocks.size(), std::size_t(1));
  if (unlocks.size() == 1) {
    KCD2S_CHECK_EQ(unlocks[0].first, link.player);
    KCD2S_CHECK(unlocks[0].second == unlock_of(9, StashUnlockMethod::Key));
  }
  KCD2S_CHECK(link.server.take_stash_unlocks().empty());
  bool opened = false;
  for (const auto& [player, report] : link.server.take_inventory_reports()) {
    opened = opened || (player == link.player && report.kind == InventoryReport::Kind::StashOpen &&
                        report.request == 9);
  }
  KCD2S_CHECK(opened);
}

KCD2S_TEST(stash_unlock, a_burst_is_bounded) {
  Link link;
  KCD2S_CHECK(link.client.mark_world_ready(1));
  link.run(5);
  for (unsigned i = 0; i < kMaxStashUnlocksPerSecond + 4; ++i) {
    KCD2S_CHECK(link.client.send_stash_unlock(unlock_of(i + 1, StashUnlockMethod::Pick)));
  }
  link.run(10);
  KCD2S_CHECK_EQ(link.server.take_stash_unlocks().size(), std::size_t(kMaxStashUnlocksPerSecond));
}
