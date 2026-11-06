#include "kcd2s/integration/pickpocket.hpp"

#include "kcd2s/testkit.hpp"

using namespace kcd2s::integration;

KCD2S_TEST(pickpocket, reports_round_trip) {
  for (const auto kind : {PickpocketReport::Kind::Request, PickpocketReport::Kind::Caught,
                          PickpocketReport::Kind::Seen, PickpocketReport::Kind::Finished}) {
    PickpocketReport report;
    report.kind = kind;
    report.victim = 4098;
    PickpocketReport decoded;
    KCD2S_CHECK(decode_pickpocket_report(encode_pickpocket_report(report), decoded));
    KCD2S_CHECK(decoded == report);
  }
  PickpocketReport none;
  none.victim = 0;
  KCD2S_CHECK(encode_pickpocket_report(none).empty());
  PickpocketReport decoded;
  KCD2S_CHECK(!decode_pickpocket_report({kPickpocketTag, 9, 1}, decoded));
  KCD2S_CHECK(!decode_pickpocket_report({kPickpocketTag, 1, 0}, decoded));
}

KCD2S_TEST(pickpocket, rules_round_trip) {
  PickpocketRule enabled;
  enabled.kind = PickpocketRule::Kind::Enabled;
  enabled.flag = false;
  PickpocketRule answer;
  answer.kind = PickpocketRule::Kind::Answer;
  answer.player = 4098;
  PickpocketRule noticed;
  noticed.kind = PickpocketRule::Kind::Noticed;
  noticed.player = 4097;
  noticed.text = "Tom\xc3\xa1\xc5\xa1 tried to pick your pocket";
  PickpocketRule stolen_marks;
  stolen_marks.kind = PickpocketRule::Kind::StolenMarks;
  stolen_marks.flag = false;
  for (const auto& rule : {enabled, answer, noticed, stolen_marks}) {
    PickpocketRule decoded;
    KCD2S_CHECK(decode_pickpocket_rule(encode_pickpocket_rule(rule), decoded));
    KCD2S_CHECK(decoded == rule);
  }
  PickpocketRule long_text = noticed;
  long_text.text.assign(kMaxPickpocketNoticeBytes + 1, 'x');
  KCD2S_CHECK(encode_pickpocket_rule(long_text).empty());
  PickpocketRule decoded;
  KCD2S_CHECK(!decode_pickpocket_rule({kPickpocketRuleTag, 5, 1, 0, 0}, decoded));
  KCD2S_CHECK(!decode_pickpocket_rule({kPickpocketRuleTag, 1, 2, 0, 0}, decoded));
}
