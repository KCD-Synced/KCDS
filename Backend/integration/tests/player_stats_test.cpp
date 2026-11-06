#include <cmath>
#include <limits>

#include "kcd2s/integration/player_stats.hpp"
#include "kcd2s/testkit.hpp"
using namespace kcd2s::integration;

// A player's stats: the "vitals" and "stats" fields and SET_STAT.
KCD2S_TEST(player_stats, vitals_round_trip) {
  PlayerVitals vitals;
  vitals.values = {100.0f, 87.256f, 95.0f, 80.5f, 0.0f, 0.0f, 1.0f, -0.25f, 12.0f};
  const std::string field = vitals_field(vitals);
  KCD2S_CHECK(field == "10000,8726,9500,8050,0,0,100,-25,1200");
  PlayerVitals parsed;
  KCD2S_CHECK(parse_vitals_field(field, parsed));
  KCD2S_CHECK(parsed.values[0] == 100.0f);
  KCD2S_CHECK(parsed.values[1] == stat_field_value(87.256f));
  KCD2S_CHECK(std::fabs(parsed.values[1] - 87.26f) < 0.0001f);
  KCD2S_CHECK(parsed.values[7] == -0.25f);
  // What the field carries is what reads back.
  KCD2S_CHECK(vitals_field(parsed) == field);
}

KCD2S_TEST(player_stats, stats_round_trip) {
  PlayerStatsSample sample;
  for (std::size_t i = 0; i < sample.derived.size(); ++i) {
    sample.derived[i] = float(i) * 1.5f - 3.0f;
  }
  sample.relative = {0.0333333f, 0.5f, 1.0f};
  const std::string field = stats_field(sample);
  PlayerStatsSample parsed;
  KCD2S_CHECK(parse_stats_field(field, parsed));
  KCD2S_CHECK(parsed.derived == sample.derived);
  KCD2S_CHECK(parsed.relative[0] == relative_field_value(0.0333333f));
  KCD2S_CHECK(std::fabs(parsed.relative[0] - 0.0333f) < 0.00001f);
  KCD2S_CHECK(parsed.relative[2] == 1.0f);
}

KCD2S_TEST(player_stats, malformed_fields_refused) {
  PlayerVitals vitals;
  vitals.values[0] = 42.0f;
  for (const char* bad :
       {"", "1,2,3", "1,2,3,4,5,6,7,8,9,10", "1,2,3,4,5,6,7,8,", ",1,2,3,4,5,6,7,8",
        "1,2,3,4,5,6,7,8,x", "1,2,3,4,5,6,7,8,1.5", "1,2,3,4,5,6,7,8,-", "1,2,3,4,5,6,7,8,+9",
        "1,2,3,4,5,6,7,8,99999999999999"}) {
    KCD2S_CHECK(!parse_vitals_field(bad, vitals));
  }
  // A refused field leaves the values alone.
  KCD2S_CHECK(vitals.values[0] == 42.0f);
  PlayerStatsSample sample;
  KCD2S_CHECK(!parse_stats_field("", sample));
  KCD2S_CHECK(!parse_stats_field(vitals_field(PlayerVitals{}), sample));
}

KCD2S_TEST(player_stats, values_not_finite_or_huge) {
  PlayerVitals vitals;
  vitals.values[0] = std::numeric_limits<float>::quiet_NaN();
  vitals.values[1] = std::numeric_limits<float>::infinity();
  vitals.values[2] = 1e20f;
  vitals.values[3] = -1e20f;
  PlayerVitals parsed;
  KCD2S_CHECK(parse_vitals_field(vitals_field(vitals), parsed));
  KCD2S_CHECK(parsed.values[0] == 0.0f);
  KCD2S_CHECK(parsed.values[1] == 0.0f);
  KCD2S_CHECK(parsed.values[2] == 1e10f);
  KCD2S_CHECK(parsed.values[3] == -1e10f);
}

KCD2S_TEST(player_stats, set_stat_round_trip) {
  StatRequest request;
  KCD2S_CHECK(decode_set_stat(encode_set_stat({1, 55.5f}), request));
  KCD2S_CHECK((request == StatRequest{1, 55.5f}));
  KCD2S_CHECK(decode_set_stat(encode_set_stat({0, 0.0f}), request) && request.value == 0.0f);
  KCD2S_CHECK(decode_set_stat(encode_set_stat({3, 1000000.0f}), request) && request.stat == 3);
  // What setStat refuses never travels and is refused on arrival.
  KCD2S_CHECK(encode_set_stat({4, 1.0f}).empty());
  KCD2S_CHECK(encode_set_stat({0, -1.0f}).empty());
  KCD2S_CHECK(encode_set_stat({0, 0.00001f}).empty());
  KCD2S_CHECK(!encode_set_stat({1, 0.00001f}).empty());
  KCD2S_CHECK(encode_set_stat({2, 1000001.0f}).empty());
  KCD2S_CHECK(encode_set_stat({2, std::numeric_limits<float>::quiet_NaN()}).empty());
  KCD2S_CHECK(!decode_set_stat({kSetStatTag, 4, 0, 0, 128, 63}, request));
  KCD2S_CHECK(!decode_set_stat({kSetStatTag, 0, 0, 0, 128, 191}, request));
  KCD2S_CHECK(!decode_set_stat({kSetStatTag, 0, 0, 0, 128}, request));
  KCD2S_CHECK(!decode_set_stat({kSetStatTag + 1, 0, 0, 0, 128, 63}, request));
}

// setHealth(0, { fall: true }): the seventh byte, only with health 0.
KCD2S_TEST(player_stats, set_stat_fall_flag) {
  StatRequest fall;
  fall.stat = 0;
  fall.value = 0.0f;
  fall.fall = true;
  const auto bytes = encode_set_stat(fall);
  KCD2S_CHECK(bytes.size() == 7 && bytes[6] == kStatFlagFall);
  StatRequest request;
  KCD2S_CHECK(decode_set_stat(bytes, request) && request == fall);
  // A six-byte SET_STAT has no fall.
  KCD2S_CHECK(decode_set_stat(encode_set_stat({0, 0.0f}), request) && !request.fall);
  // Only health 0 may fall; an unknown flag is refused.
  StatRequest wrong = fall;
  wrong.value = 50.0f;
  KCD2S_CHECK(encode_set_stat(wrong).empty());
  KCD2S_CHECK(!decode_set_stat({kSetStatTag, 0, 0, 0, 0, 0, 2}, request));
  KCD2S_CHECK(!decode_set_stat({kSetStatTag, 1, 0, 0, 0, 0, 1}, request));
}

KCD2S_TEST(player_stats, add_stat_round_trip) {
  StatRequest request;
  const StatRequest gain{2, 25.5f, true};
  KCD2S_CHECK(decode_add_stat(encode_add_stat(gain), request));
  KCD2S_CHECK(request == gain);
  KCD2S_CHECK(encode_stat_request(gain) == encode_add_stat(gain));
  const StatRequest loss{3, -1000000.0f, true};
  KCD2S_CHECK(decode_add_stat(encode_add_stat(loss), request) && request == loss);
  KCD2S_CHECK(decode_add_stat(encode_add_stat({0, 0.0f, true}), request) && request.value == 0);
  // A change is not a value: each message carries only its own kind of request.
  KCD2S_CHECK(encode_set_stat(gain).empty());
  KCD2S_CHECK(encode_add_stat({2, 25.5f}).empty());
  KCD2S_CHECK(encode_stat_request({2, 25.5f}) == encode_set_stat({2, 25.5f}));
  KCD2S_CHECK(!decode_set_stat(encode_add_stat(gain), request));
  KCD2S_CHECK(!decode_add_stat(encode_set_stat({2, 25.5f}), request));
  KCD2S_CHECK(decode_set_stat(encode_set_stat({2, 25.5f}), request) && !request.relative);
  // What addStat refuses never travels and is refused on arrival.
  KCD2S_CHECK(encode_add_stat({4, 1.0f, true}).empty());
  KCD2S_CHECK(encode_add_stat({1, 1000001.0f, true}).empty());
  KCD2S_CHECK(encode_add_stat({1, -1000001.0f, true}).empty());
  KCD2S_CHECK(encode_add_stat({1, std::numeric_limits<float>::infinity(), true}).empty());
  KCD2S_CHECK(encode_add_stat({1, std::numeric_limits<float>::quiet_NaN(), true}).empty());
  KCD2S_CHECK(!decode_add_stat({kAddStatTag, 4, 0, 0, 128, 63}, request));
  KCD2S_CHECK(!decode_add_stat({kAddStatTag, 0, 0, 0, 128}, request));
  // -1.0f (0xBF800000) is a change addStat takes, though setStat refuses it as a value.
  KCD2S_CHECK(decode_add_stat({kAddStatTag, 0, 0, 0, 128, 191}, request));
  KCD2S_CHECK(request.value == -1.0f && request.relative);
  KCD2S_CHECK(stat_request_valid({0, -1.0f, true}));
  KCD2S_CHECK(!stat_request_valid({0, -1.0f}));
}

KCD2S_TEST(player_stats, abilities_field_round_trip) {
  PlayerAbilities abilities;
  abilities.maxima = {100.0f, 126.67f, 95.5f, 100.0f, 150.0f};
  abilities.level = 5.0f;
  abilities.attributes = {5.0f, 5.5f, 10.0f};
  abilities.skills = {5.0f, 19.0f, 5.0f, 12.0f, 2.0f, 5.0f, 0.0f, 9.0f, 6.0f};
  abilities.relative_skills = {0.1667f, 0.6333f, 0.1667f, 0.4f, 0.0667f, 0.1667f, 0.0f, 0.3f, 0.2f};
  const std::string field = abilities_field(abilities);
  PlayerAbilities parsed;
  KCD2S_CHECK(parse_abilities_field(field, parsed));
  KCD2S_CHECK(abilities_field(parsed) == field);
  KCD2S_CHECK(stat_number(parsed.maxima[1]) == 126.67);
  KCD2S_CHECK(relative_number(parsed.relative_skills[1]) == 0.6333);
  KCD2S_CHECK(!parse_abilities_field(field + ",1", parsed));
  KCD2S_CHECK(!parse_abilities_field("1,2,3", parsed));
  KCD2S_CHECK(!parse_abilities_field("", parsed));
}
