#include "kcd2s/integration/server.hpp"
#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/inventory.hpp"
#include "kcd2s/testkit.hpp"
using namespace kcd2s;
using namespace kcd2s::integration;

// Batches of the changes a player's game made: epoch, order, answers and backpressure.
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
    client.mark_world_ready(1);
    run(5);
  }

  void run(int ticks) {
    for (int i = 0; i < ticks; ++i) {
      now += 1.0 / 60;
      server.world().set_tick(server.world().tick() + 1);
      server.tick(now);
      client.tick(now);
    }
  }

  // Sends a stock and returns the epoch the client got with it.
  std::uint32_t show(std::uint64_t revision) {
    InventoryShow stock;
    stock.revision = revision;
    if (!server.request_inventory_show(player, stock)) {
      return 0;
    }
    run(5);
    const auto shown = client.take_inventory_show();
    return shown ? shown->epoch : 0;
  }

  void send(std::uint32_t epoch, std::uint32_t sequence) {
    InventoryReport report;
    report.kind = InventoryReport::Kind::Changes;
    report.revision = 1;
    report.epoch = epoch;
    report.sequence = sequence;
    InventoryNativeChange lost;
    lost.kind = InventoryNativeChange::Kind::Lost;
    lost.id = "i1";
    lost.amount = 1;
    report.changes = {lost};
    client.send_inventory_report(report);
  }

  // The batches the scripts got since the last call, by sequence.
  std::vector<std::uint32_t> taken() {
    std::vector<std::uint32_t> sequences;
    for (const auto& [who, report] : server.take_inventory_reports()) {
      if (who == player && report.kind == InventoryReport::Kind::Changes) {
        sequences.push_back(report.sequence);
      }
    }
    return sequences;
  }

  std::vector<InventoryChangesAck> answers() {
    run(5);
    return client.take_inventory_changes_acks();
  }
};

InventoryChangesAck answer_of(std::uint32_t epoch, std::uint32_t sequence,
                              InventoryChangesAck::Verdict verdict, std::uint64_t revision) {
  InventoryChangesAck ack;
  ack.epoch = epoch;
  ack.sequence = sequence;
  ack.verdict = verdict;
  ack.revision = revision;
  return ack;
}

}  // namespace

KCD2S_TEST(inventory_changes, a_batch_before_the_first_stock_is_stale) {
  Link link;
  link.send(0, 1);
  const auto answers = link.answers();
  KCD2S_CHECK_EQ(answers.size(), std::size_t(1));
  if (answers.size() == 1) {
    KCD2S_CHECK(answers[0].verdict == InventoryChangesAck::Verdict::Stale);
  }
  KCD2S_CHECK(link.taken().empty());
}

KCD2S_TEST(inventory_changes, each_batch_is_taken_once_and_answered) {
  using Verdict = InventoryChangesAck::Verdict;
  Link link;
  const std::uint32_t epoch = link.show(1);
  KCD2S_CHECK(epoch != 0);
  // The epoch stays for the connection.
  KCD2S_CHECK_EQ(link.show(2), epoch);
  link.send(epoch, 1);
  link.run(5);
  KCD2S_CHECK(link.taken() == std::vector<std::uint32_t>{1});
  // The same batch again while the scripts work on it: nothing; the next one: out of order.
  link.send(epoch, 1);
  link.send(epoch, 2);
  auto answers = link.answers();
  KCD2S_CHECK_EQ(answers.size(), std::size_t(1));
  if (answers.size() == 1) {
    KCD2S_CHECK(answers[0] == answer_of(epoch, 2, Verdict::OutOfOrder, 0));
  }
  KCD2S_CHECK(link.taken().empty());
  // The scripts' answer goes to the client once; another for the same batch is refused.
  const auto applied = answer_of(epoch, 1, Verdict::Applied, 3);
  KCD2S_CHECK(link.server.request_inventory_changes_ack(link.player, applied));
  KCD2S_CHECK(!link.server.request_inventory_changes_ack(link.player, applied));
  answers = link.answers();
  KCD2S_CHECK(answers.size() == 1 && answers[0] == applied);
  // A batch sent again after its answer gets the same answer and is not taken again.
  link.send(epoch, 1);
  answers = link.answers();
  KCD2S_CHECK(answers.size() == 1 && answers[0] == applied);
  KCD2S_CHECK(link.taken().empty());
  // A gap, and another connection's epoch.
  link.send(epoch, 3);
  link.send(epoch + 1, 2);
  answers = link.answers();
  KCD2S_CHECK_EQ(answers.size(), std::size_t(2));
  if (answers.size() == 2) {
    KCD2S_CHECK(answers[0].verdict == Verdict::OutOfOrder);
    KCD2S_CHECK(answers[1].verdict == Verdict::Stale);
  }
  link.send(epoch, 2);
  link.run(5);
  KCD2S_CHECK(link.taken() == std::vector<std::uint32_t>{2});
  // An answer for a batch that is not open is refused.
  KCD2S_CHECK(!link.server.request_inventory_changes_ack(link.player,
                                                         answer_of(epoch, 1, Verdict::Applied, 4)));
}

KCD2S_TEST(inventory_changes, a_busy_server_answers_instead_of_dropping) {
  using Verdict = InventoryChangesAck::Verdict;
  Link link;
  const std::uint32_t epoch = link.show(1);
  // More reports in one second than the server takes: the batch is answered busy.
  for (int i = 0; i < 20; ++i) {
    InventoryReport shown;
    shown.kind = InventoryReport::Kind::Shown;
    shown.revision = 1;
    link.client.send_inventory_report(shown);
  }
  link.send(epoch, 1);
  auto answers = link.answers();
  KCD2S_CHECK(answers.size() == 1 && answers[0] == answer_of(epoch, 1, Verdict::Busy, 0));
  KCD2S_CHECK(link.taken().empty());
  // A second later the same batch is taken.
  link.run(70);
  link.send(epoch, 1);
  link.run(5);
  KCD2S_CHECK(link.taken() == std::vector<std::uint32_t>{1});
  KCD2S_CHECK(link.client.take_inventory_changes_acks().empty());
}

KCD2S_TEST(inventory_changes, a_batch_the_scripts_never_answer_is_refused_later) {
  using Verdict = InventoryChangesAck::Verdict;
  Link link;
  const std::uint32_t epoch = link.show(1);
  link.send(epoch, 1);
  link.run(5);
  KCD2S_CHECK(link.taken() == std::vector<std::uint32_t>{1});
  // Sent again before the time is up: still waiting.
  link.run(60 * 10);
  link.send(epoch, 1);
  KCD2S_CHECK(link.answers().empty());
  // After it: refused, and the next batch is taken.
  link.run(60 * 25);
  link.send(epoch, 1);
  const auto answers = link.answers();
  KCD2S_CHECK(answers.size() == 1 && answers[0] == answer_of(epoch, 1, Verdict::Refused, 0));
  KCD2S_CHECK(!link.server.request_inventory_changes_ack(link.player,
                                                         answer_of(epoch, 1, Verdict::Applied, 2)));
  link.send(epoch, 2);
  link.run(5);
  KCD2S_CHECK(link.taken() == std::vector<std::uint32_t>{2});
}
