#include "kcd2s/integration/gathering.hpp"

#include <limits>

#include "kcd2s/testkit.hpp"
using namespace kcd2s::integration;

// GATHER_REPORT, GATHER_VERDICT and GATHER_PLANTS.

KCD2S_TEST(gathering, reports_round_trip) {
  GatherReport report;
  report.request = 70000;
  for (std::size_t i = 0; i < report.item.size(); ++i) {
    report.item[i] = std::uint8_t(i * 13);
  }
  report.amount = 3;
  report.kind = 17;
  report.position = {2350.25f, 1980.5f, 98.75f};
  report.radius = 1.5f;
  report.plants = 2;
  GatherReport decoded;
  KCD2S_CHECK(decode_gather_report(encode_gather_report(report), decoded) && decoded == report);
}

KCD2S_TEST(gathering, refuses_reports_it_cannot_carry) {
  GatherReport report;
  report.amount = 1;
  report.plants = 1;
  report.amount = 0;
  KCD2S_CHECK(encode_gather_report(report).empty());
  report.amount = kMaxGatherAmount + 1;
  KCD2S_CHECK(encode_gather_report(report).empty());
  report.amount = 1;
  report.radius = kMaxGatherRadius + 1.0f;
  KCD2S_CHECK(encode_gather_report(report).empty());
  report.radius = 1.0f;
  report.position[1] = std::numeric_limits<float>::quiet_NaN();
  KCD2S_CHECK(encode_gather_report(report).empty());
  GatherReport decoded;
  KCD2S_CHECK(!decode_gather_report({}, decoded));
  KCD2S_CHECK(!decode_gather_report({kGatherReportTag, 1}, decoded));
}

KCD2S_TEST(gathering, verdicts_round_trip) {
  GatherVerdict verdict{12, true};
  GatherVerdict decoded;
  KCD2S_CHECK(decode_gather_verdict(encode_gather_verdict(verdict), decoded) && decoded == verdict);
  KCD2S_CHECK(!decode_gather_verdict({kGatherVerdictTag, 1, 2}, decoded));
}

KCD2S_TEST(gathering, plants_round_trip) {
  GatherPlants plants;
  plants.full = true;
  plants.picked.push_back(GatherPick{1, 2, {10.0f, 20.0f, 30.0f}, 1.25f});
  plants.picked.push_back(GatherPick{400000, 37, {-5.5f, 6.5f, 7.0f}, 0.0f});
  // A kind switched off map-wide: far wider than a player's own pick.
  plants.picked.push_back(GatherPick{9, 12, {0.0f, 0.0f, 0.0f}, kMaxGatherPickRadius});
  plants.grown = {7, 80000};
  GatherPlants decoded;
  KCD2S_CHECK(decode_gather_plants(encode_gather_plants(plants), decoded) && decoded == plants);
  plants.picked[0].radius = -1.0f;
  KCD2S_CHECK(encode_gather_plants(plants).empty());
  plants.picked[0].radius = kMaxGatherPickRadius * 2.0f;
  KCD2S_CHECK(encode_gather_plants(plants).empty());
  auto bytes = encode_gather_plants(GatherPlants{});
  KCD2S_CHECK(decode_gather_plants(bytes, decoded) && decoded.picked.empty());
  bytes.push_back(0);
  KCD2S_CHECK(!decode_gather_plants(bytes, decoded));
}
