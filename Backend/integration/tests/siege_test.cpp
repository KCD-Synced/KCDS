#include "kcd2s/integration/siege.hpp"
#include "kcd2s/testkit.hpp"

#include <limits>

using namespace kcd2s::integration;

// Siege engines: SIEGE_ENGINE, SIEGE_SHOT, SIEGE_USE and SIEGE_IMPACT.
namespace {

// 0.2.1 gave this feature 107 to 110; 111 and 112 are the ladders' and stone piles'.
static_assert(kSiegeEngineTag == 107 && kSiegeShotTag == 108 && kSiegeUseTag == 109 &&
              kSiegeImpactTag == 110);

SiegeEngineState sample_state() {
  SiegeEngineState state;
  state.cycle = SiegeCycle::Drawing;
  state.seconds = 44.0f;
  state.speed = 1.5f;
  state.rotation = {0.0f, 0.0f, 0.7071f, 0.7071f};
  state.target = {120.0f, -40.5f, 31.0f};
  state.aim_guide = true;
  state.usable = true;
  state.operated = true;
  state.mine = true;
  return state;
}

}  // namespace

KCD2S_TEST(siege, engine_round_trip) {
  SiegeEngineOrder spawn;
  spawn.kind = SiegeOrderKind::Spawn;
  spawn.id = 70001;
  spawn.engine = SiegeEngineKind::Cannon;
  spawn.position = {512.25f, 1024.5f, 40.0f};
  spawn.dimension = 0xffffffffu;
  spawn.state = sample_state();
  SiegeEngineOrder decoded;
  KCD2S_CHECK(decode_siege_engine(encode_siege_engine(spawn), decoded));
  KCD2S_CHECK(decoded == spawn);

  SiegeEngineOrder update;
  update.kind = SiegeOrderKind::Update;
  update.id = 3;
  update.state = sample_state();
  update.state.cycle = SiegeCycle::Ready;
  update.state.wrecked = true;
  update.state.mine = false;
  KCD2S_CHECK(decode_siege_engine(encode_siege_engine(update), decoded));
  KCD2S_CHECK(decoded == update);

  SiegeEngineOrder destroy;
  destroy.kind = SiegeOrderKind::Destroy;
  destroy.id = 3;
  KCD2S_CHECK(decode_siege_engine(encode_siege_engine(destroy), decoded));
  KCD2S_CHECK(decoded.kind == SiegeOrderKind::Destroy && decoded.id == 3);
}

KCD2S_TEST(siege, engine_refused) {
  SiegeEngineOrder order;
  order.kind = SiegeOrderKind::Update;
  order.id = 0;
  KCD2S_CHECK(encode_siege_engine(order).empty());
  order.id = 5;
  order.state.speed = std::numeric_limits<float>::quiet_NaN();
  KCD2S_CHECK(encode_siege_engine(order).empty());
  order.state.speed = 1.0f;
  order.state.mine = true;  // nobody works it
  KCD2S_CHECK(encode_siege_engine(order).empty());
  order.state.mine = false;
  order.state.cycle = SiegeCycle(9);
  KCD2S_CHECK(encode_siege_engine(order).empty());
  order.kind = SiegeOrderKind(7);
  order.state.cycle = SiegeCycle::Idle;
  KCD2S_CHECK(encode_siege_engine(order).empty());
  SiegeEngineOrder decoded;
  KCD2S_CHECK(!decode_siege_engine({kSiegeEngineTag, 3, 0}, decoded));
  KCD2S_CHECK(!decode_siege_engine({kSiegeEngineTag, 9, 1}, decoded));
  // A destroy with bytes after it did not come from encode_siege_engine.
  KCD2S_CHECK(!decode_siege_engine({kSiegeEngineTag, 3, 1, 0}, decoded));
}

KCD2S_TEST(siege, shot_round_trip) {
  SiegeShot shot;
  shot.engine = 70001;
  shot.shot = 12;
  shot.from = {512.0f, 1024.0f, 46.0f};
  shot.target = {600.0f, 1100.0f, 35.0f};
  shot.flight = 4.2f;
  shot.apex = 33.5f;
  shot.report = true;
  SiegeShot decoded;
  KCD2S_CHECK(decode_siege_shot(encode_siege_shot(shot), decoded));
  KCD2S_CHECK(decoded == shot);
  shot.report = false;
  KCD2S_CHECK(decode_siege_shot(encode_siege_shot(shot), decoded));
  KCD2S_CHECK(decoded == shot);
  shot.flight = -1.0f;
  KCD2S_CHECK(encode_siege_shot(shot).empty());
  shot.flight = 1.0f;
  shot.shot = 0;
  KCD2S_CHECK(encode_siege_shot(shot).empty());
}

KCD2S_TEST(siege, use_and_impact_round_trip) {
  SiegeUse use;
  use.engine = 70001;
  use.action = SiegeAction::Operate;
  SiegeUse decoded_use;
  KCD2S_CHECK(decode_siege_use(encode_siege_use(use), decoded_use));
  KCD2S_CHECK(decoded_use == use);
  use.action = SiegeAction::Fire;
  use.has_look = true;
  use.look = {0.6f, 0.8f, -0.1f};
  KCD2S_CHECK(decode_siege_use(encode_siege_use(use), decoded_use));
  KCD2S_CHECK(decoded_use == use);
  use.action = SiegeAction(4);
  KCD2S_CHECK(encode_siege_use(use).empty());
  KCD2S_CHECK(!decode_siege_use({kSiegeUseTag, 1, 4, 0}, decoded_use));
  KCD2S_CHECK(!decode_siege_use({kSiegeUseTag, 1, 0, 2}, decoded_use));

  SiegeImpact impact;
  impact.engine = 70001;
  impact.shot = 12;
  impact.position = {598.5f, 1097.0f, 34.0f};
  SiegeImpact decoded_impact;
  KCD2S_CHECK(decode_siege_impact(encode_siege_impact(impact), decoded_impact));
  KCD2S_CHECK(decoded_impact == impact);
  impact.position[0] = std::numeric_limits<float>::infinity();
  KCD2S_CHECK(encode_siege_impact(impact).empty());
}
