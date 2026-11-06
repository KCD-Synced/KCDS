#include "kcd2s/integration/server.hpp"
#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/quests.hpp"
#include "kcd2s/testkit.hpp"
using namespace kcd2s;
using namespace kcd2s::integration;

// QUEST_SHOW, QUEST_HIDE, QUEST_TRACK and QUEST_TRACKING.
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

QuestShow sample_quest() {
  QuestShow quest;
  quest.quest = 4100;
  quest.key = "find_the_smith-2";
  quest.title = "Find the smith";
  quest.description = "He went north.\nAsk at the mill.";
  quest.type = QuestType::Main;
  quest.progress = QuestProgress::Active;
  quest.announce = true;
  QuestObjective talk;
  talk.text = "Talk to the miller";
  talk.has_position = true;
  talk.position[0] = 120.5f;
  talk.position[1] = -40.25f;
  talk.position[2] = 31.0f;
  QuestObjective bread;
  bread.text = "Bring bread (optional)";
  bread.optional = true;
  bread.progress = QuestProgress::Done;
  quest.objectives = {talk, bread};
  return quest;
}

}  // namespace

KCD2S_TEST(quests, show_round_trips_through_the_codec) {
  const QuestShow quest = sample_quest();
  const auto bytes = encode_quest_show(quest);
  KCD2S_CHECK(!bytes.empty() && bytes[0] == kQuestShowTag);
  QuestShow heard;
  KCD2S_CHECK(decode_quest_show(bytes, heard));
  KCD2S_CHECK(heard == quest);

  // No objectives, every type and every progress.
  QuestShow plain = quest;
  plain.objectives.clear();
  plain.announce = false;
  for (std::uint8_t type = 0; type <= std::uint8_t(QuestType::Racing); ++type) {
    plain.type = QuestType(type);
    plain.progress = QuestProgress(type % 4);
    KCD2S_CHECK(decode_quest_show(encode_quest_show(plain), heard) && heard == plain);
  }
}

KCD2S_TEST(quests, show_refuses_what_cannot_travel) {
  QuestShow bad = sample_quest();
  bad.key = "has space";
  KCD2S_CHECK(encode_quest_show(bad).empty());
  bad.key = std::string(kQuestKeyMax + 1, 'k');
  KCD2S_CHECK(encode_quest_show(bad).empty());
  bad.key.clear();
  KCD2S_CHECK(encode_quest_show(bad).empty());

  bad = sample_quest();
  bad.quest = 0;
  KCD2S_CHECK(encode_quest_show(bad).empty());

  bad = sample_quest();
  bad.title = "line\nbreak";
  KCD2S_CHECK(encode_quest_show(bad).empty());
  bad.title = std::string(kQuestTitleMax + 1, 't');
  KCD2S_CHECK(encode_quest_show(bad).empty());

  bad = sample_quest();
  bad.type = QuestType(6);
  KCD2S_CHECK(encode_quest_show(bad).empty());
  bad = sample_quest();
  bad.progress = QuestProgress(4);
  KCD2S_CHECK(encode_quest_show(bad).empty());

  bad = sample_quest();
  bad.objectives.resize(kQuestObjectivesMax + 1);
  KCD2S_CHECK(encode_quest_show(bad).empty());
  bad = sample_quest();
  bad.objectives[0].position[1] = std::nanf("");
  KCD2S_CHECK(encode_quest_show(bad).empty());
  bad = sample_quest();
  bad.objectives[0].position[2] = kQuestCoordinateMax * 2;
  KCD2S_CHECK(encode_quest_show(bad).empty());

  // A cut, padded or retagged message is refused.
  const auto bytes = encode_quest_show(sample_quest());
  QuestShow heard;
  auto cut = bytes;
  cut.pop_back();
  KCD2S_CHECK(!decode_quest_show(cut, heard));
  auto padded = bytes;
  padded.push_back(0);
  KCD2S_CHECK(!decode_quest_show(padded, heard));
  auto other = bytes;
  other[0] = kQuestHideTag;
  KCD2S_CHECK(!decode_quest_show(other, heard));
}

KCD2S_TEST(quests, hide_and_track_round_trip) {
  std::uint32_t quest = 0;
  KCD2S_CHECK(decode_quest_hide(encode_quest_hide(77), quest) && quest == 77);
  KCD2S_CHECK(encode_quest_hide(0).empty());

  QuestTracking track{0xFFFFFFFFu, true};
  QuestTracking heard;
  KCD2S_CHECK(decode_quest_track(encode_quest_track(track), heard) && heard == track);
  track.tracked = false;
  KCD2S_CHECK(decode_quest_tracking(encode_quest_tracking(track), heard) && heard == track);
  // Track and tracking are different messages.
  KCD2S_CHECK(!decode_quest_tracking(encode_quest_track(track), heard));
  KCD2S_CHECK(encode_quest_track(QuestTracking{0, true}).empty());
  auto wrong = encode_quest_tracking(QuestTracking{5, true});
  wrong.back() = 2;
  KCD2S_CHECK(!decode_quest_tracking(wrong, heard));
}

KCD2S_TEST(quests, names_match_the_scripts) {
  QuestType type = QuestType::Side;
  KCD2S_CHECK(quest_type_of("racing", type) && type == QuestType::Racing);
  KCD2S_CHECK(std::string(quest_type_name(QuestType::Activity)) == "activity");
  KCD2S_CHECK(!quest_type_of("Main", type));
  QuestProgress progress = QuestProgress::None;
  KCD2S_CHECK(quest_progress_of("failed", progress) && progress == QuestProgress::Failed);
  KCD2S_CHECK(std::string(quest_progress_name(QuestProgress::Done)) == "done");
  KCD2S_CHECK(!quest_progress_of("finished", progress));
}

KCD2S_TEST(quests, orders_reach_the_client_in_order) {
  Link link;
  // No world yet: nothing goes out.
  KCD2S_CHECK(!link.server.send_quest_show(link.player, sample_quest()));
  KCD2S_CHECK(link.client.mark_world_ready(1));
  link.run(5);
  KCD2S_CHECK(link.server.send_quest_show(link.player, sample_quest()));
  KCD2S_CHECK(link.server.send_quest_track(link.player, QuestTracking{4100, true}));
  KCD2S_CHECK(link.server.send_quest_hide(link.player, 4100));
  link.run(5);
  const auto orders = link.client.take_quest_orders();
  KCD2S_CHECK_EQ(orders.size(), std::size_t(3));
  if (orders.size() == 3) {
    KCD2S_CHECK(orders[0].kind == QuestOrder::Kind::Show && orders[0].show == sample_quest());
    KCD2S_CHECK(orders[1].kind == QuestOrder::Kind::Track && orders[1].show.quest == 4100 &&
                orders[1].tracked);
    KCD2S_CHECK(orders[2].kind == QuestOrder::Kind::Hide && orders[2].show.quest == 4100);
  }
  KCD2S_CHECK(link.client.take_quest_orders().empty());
}

KCD2S_TEST(quests, tracking_reaches_the_server_bounded) {
  Link link;
  KCD2S_CHECK(!link.client.send_quest_tracking(QuestTracking{3, true}));
  KCD2S_CHECK(link.client.mark_world_ready(1));
  link.run(5);
  KCD2S_CHECK(link.client.send_quest_tracking(QuestTracking{3, true}));
  KCD2S_CHECK(!link.client.send_quest_tracking(QuestTracking{0, true}));
  link.run(5);
  const auto reports = link.server.take_quest_tracking();
  KCD2S_CHECK_EQ(reports.size(), std::size_t(1));
  if (reports.size() == 1) {
    KCD2S_CHECK_EQ(reports[0].first, link.player);
    KCD2S_CHECK(reports[0].second == (QuestTracking{3, true}));
  }
  // A second later the window is fresh.
  link.run(70);
  for (unsigned i = 0; i < kMaxQuestTrackingPerSecond + 5; ++i) {
    KCD2S_CHECK(link.client.send_quest_tracking(QuestTracking{i + 1, (i % 2) == 0}));
  }
  link.run(10);
  KCD2S_CHECK_EQ(link.server.take_quest_tracking().size(), std::size_t(kMaxQuestTrackingPerSecond));
}
