// Which client reports health a game would have taken off an NPC's body.
#include "npc_health.hpp"

#include "kcd2s/testkit.hpp"

using kcds::runtime::npcs::health::Body;
using kcds::runtime::npcs::health::hit_to_report;
using kcds::runtime::npcs::health::Source;

namespace {

constexpr std::uint8_t kCombat = 1;
constexpr std::uint8_t kFall = 7;

Body body(bool simulated) {
  Body b;
  b.npc = 4097;
  b.simulated = simulated;
  return b;
}

}  // namespace

KCD2S_TEST(npc_health, the_local_players_blow_is_reported_from_every_body) {
  for (const bool simulated : {false, true}) {
    const auto hit = hit_to_report(body(simulated), Source::LocalPlayer, 2, 12.5f, kCombat, 23);
    KCD2S_CHECK(hit.has_value());
    KCD2S_CHECK_EQ(hit->npc, 4097u);
    KCD2S_CHECK_EQ(hit->attacker, 2u);
    KCD2S_CHECK(hit->amount == 12.5f);
    KCD2S_CHECK_EQ(hit->reason, kCombat);
    KCD2S_CHECK_EQ(hit->subpart, 23);
  }
}

KCD2S_TEST(npc_health, the_simulator_alone_reports_falls_and_other_npcs) {
  const auto fall = hit_to_report(body(true), Source::World, 99, 30, kFall, -5);
  KCD2S_CHECK(fall.has_value());
  KCD2S_CHECK_EQ(fall->attacker, 0u);
  KCD2S_CHECK_EQ(fall->subpart, -1);
  KCD2S_CHECK(!hit_to_report(body(false), Source::World, 0, 30, kFall, -1));
  const auto npc = hit_to_report(body(true), Source::Npc, 4100, 8, kCombat, -1);
  KCD2S_CHECK(npc.has_value());
  KCD2S_CHECK_EQ(npc->attacker, 4100u);
  KCD2S_CHECK(!hit_to_report(body(false), Source::Npc, 4100, 8, kCombat, -1));
}

KCD2S_TEST(npc_health, another_players_blow_and_nothing_taken_are_not_reported) {
  KCD2S_CHECK(!hit_to_report(body(true), Source::RemotePlayer, 3, 10, kCombat, -1));
  KCD2S_CHECK(!hit_to_report(body(false), Source::RemotePlayer, 3, 10, kCombat, -1));
  KCD2S_CHECK(!hit_to_report(body(true), Source::LocalPlayer, 2, 0, kCombat, -1));
  KCD2S_CHECK(!hit_to_report(body(true), Source::LocalPlayer, 2, -1, kCombat, -1));
  KCD2S_CHECK(!hit_to_report(body(true), Source::LocalPlayer, 2, 10, 200, -1));
}
