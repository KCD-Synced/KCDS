#include "kcd2s/integration/server.hpp"
#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/dialogue.hpp"
#include "kcd2s/testkit.hpp"
using namespace kcd2s;
using namespace kcd2s::integration;

// DIALOGUE_PAGE, DIALOGUE_CLOSE, DIALOGUE_CHOICE and DIALOGUE_CANCEL: a conversation
// the server drives on one player.
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

DialoguePage page_of(std::uint32_t session, std::uint32_t generation) {
  DialoguePage page;
  page.session = session;
  page.generation = generation;
  page.on_right = true;
  // UTF-8 text travels as written (Kovac with its accents).
  page.line = "What do you want, stranger?";
  page.speaker = "Kov\xC3\xA1\xC4\x8D";
  page.options = {{"Meč na predaj", true}, {"Nič, zbohom", false}};
  return page;
}

}  // namespace

KCD2S_TEST(dialogue, a_page_round_trips_through_the_codec) {
  const DialoguePage page = page_of(7, 3);
  DialoguePage heard;
  const auto bytes = encode_dialogue_page(page);
  KCD2S_CHECK(!bytes.empty() && bytes[0] == kDialoguePageTag);
  KCD2S_CHECK(decode_dialogue_page(bytes, heard) && heard == page);
  // No line and no speaker, eight rows.
  DialoguePage bare = page_of(0xFFFFFFFFu, 1);
  bare.line.clear();
  bare.speaker.clear();
  bare.on_right = false;
  bare.options.assign(kMaxDialogueOptions, {"row", true});
  KCD2S_CHECK(decode_dialogue_page(encode_dialogue_page(bare), heard) && heard == bare);
  // What cannot travel: no session, no generation, no rows, nine rows, an empty row, a long text.
  DialoguePage bad = page_of(0, 1);
  KCD2S_CHECK(encode_dialogue_page(bad).empty());
  bad = page_of(1, 0);
  KCD2S_CHECK(encode_dialogue_page(bad).empty());
  bad = page_of(1, 1);
  bad.options.clear();
  KCD2S_CHECK(encode_dialogue_page(bad).empty());
  bad.options.assign(kMaxDialogueOptions + 1, {"row", true});
  KCD2S_CHECK(encode_dialogue_page(bad).empty());
  bad.options = {{"", true}};
  KCD2S_CHECK(encode_dialogue_page(bad).empty());
  bad.options = {{std::string(kMaxDialogueOptionText + 1, 'x'), true}};
  KCD2S_CHECK(encode_dialogue_page(bad).empty());
  bad = page_of(1, 1);
  bad.line = std::string(kMaxDialogueLine + 1, 'x');
  KCD2S_CHECK(encode_dialogue_page(bad).empty());
  // A cut or padded message, another tag.
  auto cut = bytes;
  cut.pop_back();
  KCD2S_CHECK(!decode_dialogue_page(cut, heard));
  auto padded = bytes;
  padded.push_back(0);
  KCD2S_CHECK(!decode_dialogue_page(padded, heard));
  auto other = bytes;
  other[0] = kDialogueCloseTag;
  KCD2S_CHECK(!decode_dialogue_page(other, heard));
}

KCD2S_TEST(dialogue, the_small_messages_round_trip_through_the_codec) {
  DialogueClose close;
  close.session = 42;
  DialogueClose heard_close;
  KCD2S_CHECK(decode_dialogue_close(encode_dialogue_close(close), heard_close) &&
              heard_close == close);
  KCD2S_CHECK(encode_dialogue_close(DialogueClose{}).empty());

  DialogueChoice choice;
  choice.session = 42;
  choice.generation = 9;
  choice.index = 7;
  DialogueChoice heard_choice;
  const auto choice_bytes = encode_dialogue_choice(choice);
  KCD2S_CHECK(decode_dialogue_choice(choice_bytes, heard_choice) && heard_choice == choice);
  choice.index = 8;
  KCD2S_CHECK(encode_dialogue_choice(choice).empty());
  auto wrong = choice_bytes;
  wrong.back() = 8;
  KCD2S_CHECK(!decode_dialogue_choice(wrong, heard_choice));

  DialogueCancel cancel;
  cancel.session = 42;
  cancel.reason = DialogueCloseReason::Interrupted;
  DialogueCancel heard_cancel;
  const auto cancel_bytes = encode_dialogue_cancel(cancel);
  KCD2S_CHECK(decode_dialogue_cancel(cancel_bytes, heard_cancel) && heard_cancel == cancel);
  // Only the player's side reasons travel: completed and replaced are the server's.
  cancel.reason = DialogueCloseReason::Replaced;
  KCD2S_CHECK(encode_dialogue_cancel(cancel).empty());
  auto completed = cancel_bytes;
  completed.back() = 0;
  KCD2S_CHECK(!decode_dialogue_cancel(completed, heard_cancel));
}

KCD2S_TEST(dialogue, pages_and_closes_reach_the_player_in_order) {
  Link link;
  // No ready world yet: the page reaches nobody.
  KCD2S_CHECK(!link.server.send_dialogue_page(link.player, page_of(1, 1)));
  KCD2S_CHECK(link.client.mark_world_ready(1));
  link.run(5);
  KCD2S_CHECK(link.server.send_dialogue_page(link.player, page_of(1, 1)));
  KCD2S_CHECK(link.server.send_dialogue_page(link.player, page_of(1, 2)));
  DialogueClose close;
  close.session = 1;
  KCD2S_CHECK(link.server.send_dialogue_close(link.player, close));
  KCD2S_CHECK(!link.server.send_dialogue_page(link.player + 1, page_of(1, 3)));
  link.run(5);
  const auto orders = link.client.take_dialogue_orders();
  KCD2S_CHECK_EQ(orders.size(), std::size_t(3));
  if (orders.size() == 3) {
    KCD2S_CHECK(std::holds_alternative<DialoguePage>(orders[0]) &&
                std::get<DialoguePage>(orders[0]) == page_of(1, 1));
    KCD2S_CHECK(std::holds_alternative<DialoguePage>(orders[1]) &&
                std::get<DialoguePage>(orders[1]).generation == 2);
    KCD2S_CHECK(std::holds_alternative<DialogueClose>(orders[2]) &&
                std::get<DialogueClose>(orders[2]) == close);
  }
  KCD2S_CHECK(link.client.take_dialogue_orders().empty());
}

KCD2S_TEST(dialogue, picks_and_cancels_reach_the_server_bounded) {
  Link link;
  DialogueChoice choice;
  choice.session = 5;
  choice.generation = 2;
  choice.index = 1;
  KCD2S_CHECK(!link.client.send_dialogue_choice(choice));
  KCD2S_CHECK(link.client.mark_world_ready(1));
  link.run(5);
  KCD2S_CHECK(link.client.send_dialogue_choice(choice));
  DialogueCancel cancel;
  cancel.session = 5;
  cancel.reason = DialogueCloseReason::Cancelled;
  KCD2S_CHECK(link.client.send_dialogue_cancel(cancel));
  link.run(5);
  const auto choices = link.server.take_dialogue_choices();
  KCD2S_CHECK_EQ(choices.size(), std::size_t(1));
  if (choices.size() == 1) {
    KCD2S_CHECK_EQ(choices[0].first, link.player);
    KCD2S_CHECK(choices[0].second == choice);
  }
  const auto cancels = link.server.take_dialogue_cancels();
  KCD2S_CHECK_EQ(cancels.size(), std::size_t(1));
  if (cancels.size() == 1) {
    KCD2S_CHECK(cancels[0].second == cancel);
  }
  link.run(70);
  for (unsigned i = 0; i < kMaxDialogueReportsPerSecond + 4; ++i) {
    KCD2S_CHECK(link.client.send_dialogue_choice(choice));
  }
  link.run(10);
  KCD2S_CHECK_EQ(link.server.take_dialogue_choices().size(),
                 std::size_t(kMaxDialogueReportsPerSecond));
}
