#include "kcd2s/integration/ladders_stones.hpp"
#include "kcd2s/testkit.hpp"

#include <limits>

using namespace kcd2s::integration;

// Siege ladders and stone piles: SIEGE_LADDER and STONE_PILE.
namespace {

static_assert(kSiegeLadderTag == 111 && kStonePileTag == 112);

SiegeLadderOrder sample_spawn() {
  SiegeLadderOrder spawn;
  spawn.kind = LadderMessageKind::Spawn;
  spawn.id = 80001;
  spawn.ladder = LadderKind::Tall;
  spawn.height = 5.5f;
  spawn.lean = 12.0f;
  spawn.position = {512.25f, 1024.5f, 40.0f};
  spawn.rotation = {0.0f, 0.0f, 0.7071f, 0.7071f};
  spawn.dimension = 0xffffffffu;
  spawn.pose = LadderPose::Down;
  spawn.usable = true;
  return spawn;
}

}  // namespace

KCD2S_TEST(ladders_stones, ladder_round_trip) {
  const SiegeLadderOrder spawn = sample_spawn();
  SiegeLadderOrder decoded;
  KCD2S_CHECK(decode_siege_ladder(encode_siege_ladder(spawn), decoded));
  KCD2S_CHECK(decoded == spawn);

  SiegeLadderOrder pose;
  pose.kind = LadderMessageKind::Pose;
  pose.id = 80001;
  pose.pose = LadderPose::Pushed;
  pose.usable = false;
  pose.mine = true;
  KCD2S_CHECK(decode_siege_ladder(encode_siege_ladder(pose), decoded));
  KCD2S_CHECK(decoded == pose);

  SiegeLadderOrder destroy;
  destroy.kind = LadderMessageKind::Destroy;
  destroy.id = 80001;
  KCD2S_CHECK(decode_siege_ladder(encode_siege_ladder(destroy), decoded));
  KCD2S_CHECK(decoded.kind == LadderMessageKind::Destroy && decoded.id == 80001);

  SiegeLadderOrder refused;
  refused.kind = LadderMessageKind::Refused;
  KCD2S_CHECK(encode_siege_ladder(refused).size() == 2);
  KCD2S_CHECK(decode_siege_ladder(encode_siege_ladder(refused), decoded));
  KCD2S_CHECK(decoded.kind == LadderMessageKind::Refused);
}

KCD2S_TEST(ladders_stones, ladder_refused) {
  SiegeLadderOrder spawn = sample_spawn();
  spawn.id = 0;
  KCD2S_CHECK(encode_siege_ladder(spawn).empty());
  spawn = sample_spawn();
  spawn.height = 0.0f;
  KCD2S_CHECK(encode_siege_ladder(spawn).empty());
  spawn = sample_spawn();
  spawn.lean = std::numeric_limits<float>::quiet_NaN();
  KCD2S_CHECK(encode_siege_ladder(spawn).empty());
  spawn = sample_spawn();
  spawn.ladder = LadderKind(4);
  KCD2S_CHECK(encode_siege_ladder(spawn).empty());
  spawn = sample_spawn();
  spawn.pose = LadderPose(5);
  KCD2S_CHECK(encode_siege_ladder(spawn).empty());
  // A client's report is not a server's order.
  spawn = sample_spawn();
  spawn.kind = LadderMessageKind::Use;
  KCD2S_CHECK(encode_siege_ladder(spawn).empty());
  SiegeLadderOrder decoded;
  KCD2S_CHECK(!decode_siege_ladder({kSiegeLadderTag, 3, 0}, decoded));
  KCD2S_CHECK(!decode_siege_ladder({kSiegeLadderTag, 2, 1, 5, 0}, decoded));
  KCD2S_CHECK(!decode_siege_ladder({kSiegeLadderTag, 2, 1, 0, 4}, decoded));
  KCD2S_CHECK(!decode_siege_ladder({kSiegeLadderTag, 3, 1, 0}, decoded));
  KCD2S_CHECK(!decode_siege_ladder({kSiegeLadderTag, 4, 0}, decoded));
  KCD2S_CHECK(!decode_siege_ladder({kSiegeLadderTag, 5, 1, 0}, decoded));
}

KCD2S_TEST(ladders_stones, ladder_report_round_trip) {
  SiegeLadderReport use;
  use.kind = LadderMessageKind::Use;
  use.ladder = 80001;
  use.action = LadderAction::Raise;
  SiegeLadderReport decoded;
  KCD2S_CHECK(decode_siege_ladder_report(encode_siege_ladder_report(use), decoded));
  KCD2S_CHECK(decoded == use);

  SiegeLadderReport settled;
  settled.kind = LadderMessageKind::Settled;
  settled.ladder = 80001;
  settled.pose = LadderPose::Standing;
  KCD2S_CHECK(decode_siege_ladder_report(encode_siege_ladder_report(settled), decoded));
  KCD2S_CHECK(decoded == settled);

  settled.pose = LadderPose::Broken;
  KCD2S_CHECK(encode_siege_ladder_report(settled).empty());
  use.action = LadderAction(2);
  KCD2S_CHECK(encode_siege_ladder_report(use).empty());
  KCD2S_CHECK(!decode_siege_ladder_report({kSiegeLadderTag, 5, 1, 2}, decoded));
  KCD2S_CHECK(!decode_siege_ladder_report({kSiegeLadderTag, 6, 1, 1}, decoded));
  KCD2S_CHECK(!decode_siege_ladder_report({kSiegeLadderTag, 2, 1, 0}, decoded));
  KCD2S_CHECK(!decode_siege_ladder_report({kSiegeLadderTag, 5, 1, 0, 0}, decoded));
}

KCD2S_TEST(ladders_stones, pile_round_trip) {
  StonePileOrder spawn;
  spawn.kind = StonePileMessageKind::Spawn;
  spawn.id = 90001;
  spawn.position = {100.0f, 200.0f, 30.5f};
  spawn.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
  spawn.dimension = 4;
  spawn.stones = -1;
  spawn.usable = true;
  StonePileOrder decoded;
  KCD2S_CHECK(decode_stone_pile(encode_stone_pile(spawn), decoded));
  KCD2S_CHECK(decoded == spawn);

  StonePileOrder update;
  update.kind = StonePileMessageKind::Update;
  update.id = 90001;
  update.stones = 0;
  update.usable = false;
  KCD2S_CHECK(decode_stone_pile(encode_stone_pile(update), decoded));
  KCD2S_CHECK(decoded == update);
  update.stones = kMaxPileStones;
  KCD2S_CHECK(decode_stone_pile(encode_stone_pile(update), decoded));
  KCD2S_CHECK(decoded == update);

  update.stones = -2;
  KCD2S_CHECK(encode_stone_pile(update).empty());
  update.stones = kMaxPileStones + 1;
  KCD2S_CHECK(encode_stone_pile(update).empty());
  KCD2S_CHECK(!decode_stone_pile({kStonePileTag, 2, 1, 1, 2}, decoded));
  KCD2S_CHECK(!decode_stone_pile({kStonePileTag, 3, 1, 0}, decoded));
  KCD2S_CHECK(!decode_stone_pile({kStonePileTag, 4, 1}, decoded));
}

KCD2S_TEST(ladders_stones, pile_report_round_trip) {
  StonePileReport take;
  take.kind = StonePileMessageKind::Take;
  take.pile = 90001;
  StonePileReport decoded;
  KCD2S_CHECK(decode_stone_pile_report(encode_stone_pile_report(take), decoded));
  KCD2S_CHECK(decoded == take);

  StonePileReport impact;
  impact.kind = StonePileMessageKind::Impact;
  impact.pile = 90001;
  impact.position = {103.0f, 190.0f, 22.0f};
  KCD2S_CHECK(decode_stone_pile_report(encode_stone_pile_report(impact), decoded));
  KCD2S_CHECK(decoded == impact);

  impact.position[2] = std::numeric_limits<float>::infinity();
  KCD2S_CHECK(encode_stone_pile_report(impact).empty());
  take.kind = StonePileMessageKind::Update;
  KCD2S_CHECK(encode_stone_pile_report(take).empty());
  KCD2S_CHECK(!decode_stone_pile_report({kStonePileTag, 1, 1}, decoded));
  KCD2S_CHECK(!decode_stone_pile_report({kStonePileTag, 4, 1, 0}, decoded));
}
