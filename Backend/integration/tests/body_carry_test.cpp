#include "kcd2s/integration/body_carry.hpp"

#include "kcd2s/testkit.hpp"

using namespace kcd2s::integration;

KCD2S_TEST(body_carry, reports_round_trip) {
  for (const auto kind : {BodyCarryReport::Kind::Started, BodyCarryReport::Kind::Ended}) {
    BodyCarryReport report;
    report.kind = kind;
    report.carried = 4099;
    BodyCarryReport decoded;
    KCD2S_CHECK(decode_body_carry_report(encode_body_carry_report(report), decoded));
    KCD2S_CHECK(decoded == report);
  }
  BodyCarryReport nobody;
  KCD2S_CHECK(encode_body_carry_report(nobody).empty());
  BodyCarryReport decoded;
  KCD2S_CHECK(!decode_body_carry_report({kBodyCarryReportTag, 3, 1}, decoded));
}

KCD2S_TEST(body_carry, carries_round_trip) {
  for (const auto kind : {BodyCarry::Kind::Carry, BodyCarry::Kind::Drop, BodyCarry::Kind::PutDown,
                          BodyCarry::Kind::Refuse}) {
    BodyCarry carry;
    carry.kind = kind;
    carry.carrier = 4100;
    carry.carried = 4099;
    BodyCarry decoded;
    KCD2S_CHECK(decode_body_carry(encode_body_carry(carry), decoded));
    KCD2S_CHECK(decoded == carry);
  }
  BodyCarry missing;
  missing.carrier = 4100;
  KCD2S_CHECK(encode_body_carry(missing).empty());
  BodyCarry decoded;
  KCD2S_CHECK(!decode_body_carry({kBodyCarryTag, 5, 1, 1}, decoded));
}
