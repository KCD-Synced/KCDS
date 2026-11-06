#include "kcd2s/integration/grime.hpp"

#include "kcd2s/testkit.hpp"

using namespace kcd2s::integration;

KCD2S_TEST(grime, orders_round_trip) {
  GrimeOrder clean;
  clean.clothes = false;
  GrimeOrder soil;
  soil.kind = GrimeOrder::Kind::Soil;
  soil.dirt = 200;
  soil.blood = 64;
  for (const auto& order : {clean, soil}) {
    GrimeOrder decoded;
    KCD2S_CHECK(decode_grime_order(encode_grime_order(order), decoded));
    KCD2S_CHECK(decoded == order);
  }
  GrimeOrder nothing;
  nothing.body = false;
  nothing.clothes = false;
  KCD2S_CHECK(encode_grime_order(nothing).empty());
  GrimeOrder decoded;
  KCD2S_CHECK(!decode_grime_order({kGrimeOrderTag, 3, 1, 0, 0}, decoded));
  KCD2S_CHECK(!decode_grime_order({kGrimeOrderTag, 1, 0, 0, 0}, decoded));
  KCD2S_CHECK(!decode_grime_order({kGrimeOrderTag, 1, 1, 0}, decoded));
}

KCD2S_TEST(grime, field_parses_body_and_pieces) {
  std::string field = "KG1";
  field += std::string("\x80\x01\x02\x03\x04\x05\x06", 7);
  field += std::string(16, '\x11');
  field += std::string("\xff\x00\x00\x00\x00\x00\x09\xc0", 8);
  Grime grime;
  KCD2S_CHECK(parse_grime_field(field, grime));
  KCD2S_CHECK(grime.body.dirt == 0x80);
  KCD2S_CHECK(grime.body.blood[5] == 6);
  KCD2S_CHECK(grime.pieces.size() == 1);
  KCD2S_CHECK(grime.pieces[0].item[0] == 0x11);
  KCD2S_CHECK(grime.pieces[0].state.dirt == 0xff);
  KCD2S_CHECK(grime.pieces[0].state.blood[5] == 9);
  KCD2S_CHECK(grime.pieces[0].wear == 0xc0);
  KCD2S_CHECK(!parse_grime_field("", grime));
  KCD2S_CHECK(!parse_grime_field(field.substr(0, field.size() - 1), grime));
}
