#include "kcd2s/integration/graves.hpp"

#include "kcd2s/testkit.hpp"

using namespace kcd2s::integration;

KCD2S_TEST(graves, every_kind_round_trips) {
  for (const GraveKind kind :
       {GraveKind::Report, GraveKind::Dug, GraveKind::Set, GraveKind::Digging}) {
    Grave grave;
    grave.kind = kind;
    grave.position[0] = 318710;
    grave.position[1] = -49590;
    grave.position[2] = 4050;
    grave.dug = true;
    const auto bytes = encode_grave(grave);
    KCD2S_CHECK(!bytes.empty() && bytes[0] == kGraveTag);
    Grave decoded;
    KCD2S_CHECK(decode_grave(bytes, decoded));
    KCD2S_CHECK(decoded == grave);
  }
}

// A digging that stopped short closes the soil again: kDigging travels with dug 0 too.
KCD2S_TEST(graves, digging_travels_open_and_closed) {
  for (const bool open : {true, false}) {
    Grave grave;
    grave.kind = GraveKind::Digging;
    grave.position[0] = 312201;
    grave.dug = open;
    Grave decoded;
    KCD2S_CHECK(decode_grave(encode_grave(grave), decoded));
    KCD2S_CHECK(decoded == grave);
  }
}

KCD2S_TEST(graves, bad_graves_do_not_travel) {
  Grave dug_but_full;
  dug_but_full.kind = GraveKind::Dug;
  dug_but_full.dug = false;
  KCD2S_CHECK(encode_grave(dug_but_full).empty());
  Grave far_away;
  far_away.position[0] = kMaxGraveCentimetres + 1;
  KCD2S_CHECK(encode_grave(far_away).empty());
  Grave grave;
  auto bytes = encode_grave(grave);
  bytes.push_back(0);
  Grave decoded;
  KCD2S_CHECK(!decode_grave(bytes, decoded));
  bytes = encode_grave(grave);
  bytes[1] = 7;
  KCD2S_CHECK(!decode_grave(bytes, decoded));
  bytes = encode_grave(grave);
  bytes.back() = 2;
  KCD2S_CHECK(!decode_grave(bytes, decoded));
  KCD2S_CHECK(!decode_grave({kGraveTag}, decoded));
}

KCD2S_TEST(graves, ids_are_the_position_in_centimetres) {
  const std::int64_t position[3] = {318710, -49590, 4050};
  KCD2S_CHECK(grave_id(position) == "318710,-49590,4050");
  std::int64_t parsed[3] = {};
  KCD2S_CHECK(parse_grave_id("318710,-49590,4050", parsed));
  KCD2S_CHECK(parsed[0] == 318710 && parsed[1] == -49590 && parsed[2] == 4050);
  KCD2S_CHECK(!parse_grave_id("1,2", parsed));
  KCD2S_CHECK(!parse_grave_id("1,2,3,", parsed));
  KCD2S_CHECK(!parse_grave_id("1,,3", parsed));
  KCD2S_CHECK(!parse_grave_id("a,2,3", parsed));
  KCD2S_CHECK(!parse_grave_id("1234567890,2,3", parsed));
}
