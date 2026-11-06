#include "kcd2s/integration/cookpot.hpp"

#include "kcd2s/testkit.hpp"
using namespace kcd2s::integration;

// POT_LOOKS.

KCD2S_TEST(cookpot, looks_round_trip) {
  PotLooks looks;
  PotLook home;
  home.cauldron = 0x9ac1dfdabb420f49ull;
  home.phase = 3;
  home.fire = kPotFireLit;  // held lit by the server
  PotLook camp;
  camp.cauldron = 0xf9c0fb3999800805ull;
  camp.camp = true;
  camp.phase = kMinPotPhase;
  looks.looks = {home, camp};
  PotLooks decoded;
  KCD2S_CHECK(decode_pot_looks(encode_pot_looks(looks), decoded) && decoded == looks);
}

KCD2S_TEST(cookpot, refuses_looks_it_cannot_carry) {
  PotLooks looks;
  KCD2S_CHECK(encode_pot_looks(looks).empty());
  PotLook look;
  look.cauldron = 1;
  look.phase = kMaxPotPhase + 1;
  looks.looks = {look};
  KCD2S_CHECK(encode_pot_looks(looks).empty());
  looks.looks[0].phase = 0;
  KCD2S_CHECK(encode_pot_looks(looks).empty());
  looks.looks[0].phase = 2;
  looks.looks[0].cauldron = 0;
  KCD2S_CHECK(encode_pot_looks(looks).empty());
  looks.looks.assign(kMaxPotLooks + 1, PotLook{1, 0, false, 2});
  KCD2S_CHECK(encode_pot_looks(looks).empty());
  looks.looks = {PotLook{1, 0, false, 2, kPotFireOut + 1}};
  KCD2S_CHECK(encode_pot_looks(looks).empty());
}

KCD2S_TEST(cookpot, refuses_malformed_looks) {
  PotLooks looks;
  looks.looks = {PotLook{7, 0x4edd0a82fc280da8ull, true, 5}};
  auto bytes = encode_pot_looks(looks);
  PotLooks decoded;
  KCD2S_CHECK(!decode_pot_looks({}, decoded));
  auto shorter = bytes;
  shorter.pop_back();
  KCD2S_CHECK(!decode_pot_looks(shorter, decoded));
  auto longer = bytes;
  longer.push_back(0);
  KCD2S_CHECK(!decode_pot_looks(longer, decoded));
  // A camp flag that is neither 0 nor 1, a fire that is none of the three.
  auto flag = bytes;
  flag[flag.size() - 3] = 2;
  KCD2S_CHECK(!decode_pot_looks(flag, decoded));
  auto fire = bytes;
  fire[fire.size() - 1] = kPotFireOut + 1;
  KCD2S_CHECK(!decode_pot_looks(fire, decoded));
}
