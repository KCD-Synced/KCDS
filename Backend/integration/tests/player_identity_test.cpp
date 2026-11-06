#include "kcd2s/integration/player_identity.hpp"
#include "kcd2s/testkit.hpp"
using namespace kcd2s::integration;

// Player names: NICKNAME carries a client's name, the server keeps the clean one.
KCD2S_TEST(player_identity, nickname_round_trip) {
  std::string name;
  KCD2S_CHECK(decode_nickname(encode_nickname("Jindřich"), name) && name == "Jindřich");
  KCD2S_CHECK(encode_nickname("").empty());
  KCD2S_CHECK(encode_nickname(std::string(kNicknameMaxBytes + 1, 'a')).empty());
  KCD2S_CHECK(!decode_nickname({kNicknameTag, 3, 'a', 'b'}, name));
  KCD2S_CHECK(!decode_nickname({kNicknameTag, 0}, name));
  KCD2S_CHECK(!decode_nickname({kNicknameTag + 1, 1, 'a'}, name));
}

KCD2S_TEST(player_identity, nickname_cleaned) {
  KCD2S_CHECK(clean_nickname("Henry") == "Henry");
  KCD2S_CHECK(clean_nickname("  Henry   of   Skalitz  ") == "Henry of Skalitz");
  KCD2S_CHECK(clean_nickname("Jind\xC5\x99ich") == "Jindřich");
  // Control and direction characters go; a name of only those is no name.
  KCD2S_CHECK(clean_nickname("He\tn\nry\x7F") == "Henry");
  KCD2S_CHECK(clean_nickname("\xE2\x80\xAE"
                             "evil") == "evil");
  KCD2S_CHECK(clean_nickname("\x01\x02 ").empty());
  // Invalid UTF-8 (a lone continuation byte, an overlong form, a surrogate) is refused whole.
  KCD2S_CHECK(clean_nickname("bad\x80").empty());
  KCD2S_CHECK(clean_nickname("\xC0\xAF").empty());
  KCD2S_CHECK(clean_nickname("\xED\xA0\x80").empty());
  // At most 32 characters, counted as characters, not bytes.
  KCD2S_CHECK(clean_nickname(std::string(40, 'a')) == std::string(32, 'a'));
  std::string long_name;
  for (int i = 0; i < 40; ++i) {
    long_name += "ř";
  }
  std::string kept;
  for (int i = 0; i < 32; ++i) {
    kept += "ř";
  }
  KCD2S_CHECK(clean_nickname(long_name) == kept);
}
