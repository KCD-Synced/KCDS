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

KCD2S_TEST(npc_health, the_hud_names_an_npc_by_its_name_and_keeps_the_unconscious_mark) {
  using namespace kcds::runtime::npcs::health;
  clear();
  track(77, nullptr, body(true));
  KCD2S_CHECK(!hud_name_of_entity(77));
  set_hud_name(4097, "Zelinář Gustav");
  KCD2S_CHECK(hud_name_of_entity(77) == std::optional<std::string>("Zelinář Gustav"));
  KCD2S_CHECK(!hud_name_of_entity(78));
  KCD2S_CHECK_EQ(hud_title("ui_nm_craftsman", "Zelinář Gustav"), std::string("Zelinář Gustav"));
  KCD2S_CHECK_EQ(hud_title("ui_nm_craftsman ui_body_unconscious", "Gustav"),
                 std::string("Gustav ui_body_unconscious"));
  KCD2S_CHECK_EQ(hud_shown_title("Zelinář Gustav"), std::string("Zelinář Gustav"));
  KCD2S_CHECK_EQ(hud_shown_title("Kovar Jan ui_body_unconscious"),
                 std::string("Kovar Jan @ui_body_unconscious"));
  set_hud_name(4097, "");
  KCD2S_CHECK(!hud_name_of_entity(77));
  set_hud_name(4097, "Gustav");
  forget(4097);
  KCD2S_CHECK(!hud_name_of_entity(77));
  clear();
}
