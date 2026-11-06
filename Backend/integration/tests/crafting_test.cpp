#include "kcd2s/integration/crafting.hpp"

#include <limits>

#include "kcd2s/testkit.hpp"
using namespace kcd2s::integration;

// CRAFT_REPORT and CRAFT_VERDICT.

KCD2S_TEST(crafting, reports_round_trip) {
  CraftReport ask;
  ask.kind = CraftReport::Kind::Ask;
  ask.request = 70000;
  ask.craft = CraftKind::Smithing;
  ask.station = {1997.125f, 2498.75f, 115.0625f};
  CraftReport decoded;
  KCD2S_CHECK(decode_craft_report(encode_craft_report(ask), decoded) && decoded == ask);
  CraftReport ended;
  ended.kind = CraftReport::Kind::Ended;
  ended.craft = CraftKind::Alchemy;
  ended.station = {2299.5f, 2016.375f, 106.375f};
  ended.reason = "cancelled";
  KCD2S_CHECK(decode_craft_report(encode_craft_report(ended), decoded) && decoded == ended);
  // The longest smithing recipe id of the game fits.
  CraftReport recipe;
  recipe.kind = CraftReport::Kind::Recipe;
  recipe.craft = CraftKind::Smithing;
  recipe.station = {1997.125f, 2498.75f, 115.0625f};
  recipe.reason = "r_kovaniKovarskaSoutez_huntingKnifeForContest";
  KCD2S_CHECK(decode_craft_report(encode_craft_report(recipe), decoded) && decoded == recipe);
  // The game broke the smith's workpiece.
  CraftReport failed;
  failed.kind = CraftReport::Kind::Failed;
  failed.craft = CraftKind::Smithing;
  failed.station = recipe.station;
  KCD2S_CHECK(decode_craft_report(encode_craft_report(failed), decoded) && decoded == failed);
  // Smoking at a smokehouse, asked of the server.
  CraftReport prepare;
  prepare.kind = CraftReport::Kind::Prepare;
  prepare.request = 0x40000001u;
  prepare.station = {2038.375f, 2489.75f, 114.875f};
  prepare.reason = "smoking";
  KCD2S_CHECK(decode_craft_report(encode_craft_report(prepare), decoded) && decoded == prepare);
  // No kind past Prepare travels.
  auto bytes = encode_craft_report(prepare);
  bytes[1] = 7;
  KCD2S_CHECK(!decode_craft_report(bytes, decoded));
}

KCD2S_TEST(crafting, refuses_what_it_cannot_carry) {
  CraftReport report;
  report.reason = "has space";
  KCD2S_CHECK(encode_craft_report(report).empty());
  report.reason = std::string(kMaxCraftReasonBytes + 1, 'a');
  KCD2S_CHECK(encode_craft_report(report).empty());
  report.reason.clear();
  report.station[0] = std::numeric_limits<float>::quiet_NaN();
  KCD2S_CHECK(encode_craft_report(report).empty());
  CraftReport decoded;
  KCD2S_CHECK(!decode_craft_report({}, decoded));
  KCD2S_CHECK(!decode_craft_report({kCraftReportTag, 9}, decoded));
  auto bytes = encode_craft_report(CraftReport{});
  bytes.push_back(0);
  KCD2S_CHECK(!decode_craft_report(bytes, decoded));
}

KCD2S_TEST(crafting, verdicts_round_trip) {
  CraftVerdict verdict{12, true};
  CraftVerdict decoded;
  KCD2S_CHECK(decode_craft_verdict(encode_craft_verdict(verdict), decoded) && decoded == verdict);
  verdict.allowed = false;
  KCD2S_CHECK(decode_craft_verdict(encode_craft_verdict(verdict), decoded) && decoded == verdict);
  KCD2S_CHECK(!decode_craft_verdict({kCraftVerdictTag, 1, 2}, decoded));
  KCD2S_CHECK(!decode_craft_verdict({kCraftReportTag, 1, 1}, decoded));
}
