#include "priced_hits.hpp"

#include <cstring>

#include "kcd2s/testkit.hpp"

namespace {

using kcd2s::integration::HitModifiers;
using kcd2s::integration::HitRuling;
using kcd2s::integration::HitVerdict;
using namespace kcds::runtime::combat;

template <std::size_t N>
void put(std::array<std::uint8_t, N>& bytes, std::size_t offset, float value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

template <std::size_t N>
float get(const std::array<std::uint8_t, N>& bytes, std::size_t offset) {
  float value = 0;
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

// A slash on the head, blocked: attack 40 in the slash pass, armour 10/12/8, block 21.
PriceInput head_slash() {
  PriceInput input;
  put(input.bytes, kPriceAttack + 4, 40.0f);
  put(input.bytes, kPriceArmor, 10.0f);
  put(input.bytes, kPriceArmor + 4, 12.0f);
  put(input.bytes, kPriceArmor + 8, 8.0f);
  for (std::size_t pass = 0; pass < 3; ++pass) {
    put(input.bytes, kPriceBlock + pass * 4, 21.0f);
  }
  put(input.bytes, kPriceCoefficient, 1.5f);
  return input;
}

HitVerdict priced(float attack, float defense, float block, float health) {
  HitVerdict verdict{1, HitRuling::Priced, 0, HitModifiers{attack, defense, block, health}};
  return verdict;
}

}  // namespace

KCD2S_TEST(priced_hits, the_price_shows_the_numbers_the_game_priced_the_blow_with) {
  Calculator calculator;
  put(calculator.bytes, kCalculatorBlockDefense, 21.0f);
  PriceResult result;
  result.bytes[kResultBlockBroken] = 1;
  const auto price = hit_price_of(calculator, head_slash(), result, 24);
  KCD2S_CHECK(price.attack[0] == 0.0f && price.attack[1] == 40.0f && price.attack[2] == 0.0f);
  KCD2S_CHECK(price.armor[0] == 10.0f && price.armor[1] == 12.0f && price.armor[2] == 8.0f);
  KCD2S_CHECK(price.blocked && price.block_defense == 21.0f && price.block_broken);
  KCD2S_CHECK(!price.perfect_block && price.body_coefficient == 1.5f && price.subpart == 24);
}

KCD2S_TEST(priced_hits, an_unblocked_blow_shows_no_block_and_odd_numbers_read_zero) {
  Calculator calculator;
  put(calculator.bytes, kCalculatorBlockDefense, -1.0f);
  calculator.bytes[kCalculatorPerfectBlock] = 1;
  auto input = head_slash();
  put(input.bytes, kPriceArmor, -3.0f);
  const auto price = hit_price_of(calculator, input, PriceResult{}, 70000);
  KCD2S_CHECK(!price.blocked && price.block_defense == 0.0f && price.perfect_block);
  KCD2S_CHECK(price.armor[0] == 0.0f && price.subpart == -1 && !price.block_broken);
}

KCD2S_TEST(priced_hits, modifiers_scale_attack_armour_and_block_in_every_pass) {
  const auto input = modified_input(head_slash(), HitModifiers{2.0f, 0.5f, 0.0f, 3.0f});
  KCD2S_CHECK(get(input.bytes, kPriceAttack + 4) == 80.0f);
  KCD2S_CHECK(get(input.bytes, kPriceArmor) == 5.0f && get(input.bytes, kPriceArmor + 8) == 4.0f);
  KCD2S_CHECK(get(input.bytes, kPriceBlock) == 0.0f && get(input.bytes, kPriceBlock + 8) == 0.0f);
  // The coefficient and anything else stay.
  KCD2S_CHECK(get(input.bytes, kPriceCoefficient) == 1.5f);
}

KCD2S_TEST(priced_hits, rulings_set_scale_or_refuse_the_health_the_game_took) {
  KCD2S_CHECK(ruled_priced_health(HitVerdict{1, HitRuling::Refused, 0}, 12.0f, 0, 0) == 0.0f);
  KCD2S_CHECK(ruled_priced_health(HitVerdict{1, HitRuling::Native, 0}, 12.0f, 0, 0) == 12.0f);
  KCD2S_CHECK(ruled_priced_health(HitVerdict{1, HitRuling::Set, 30.0f}, 12.0f, 0, 0) == 30.0f);
  KCD2S_CHECK(ruled_priced_health(HitVerdict{1, HitRuling::Scaled, 0.5f}, 12.0f, 0, 0) == 6.0f);
}

KCD2S_TEST(priced_hits, a_priced_ruling_takes_the_game_price_with_the_modifiers) {
  // The game took its price: the re-priced blow takes the re-price, times the health scale.
  KCD2S_CHECK(ruled_priced_health(priced(2, 1, 1, 1), 12.0f, 12.0f, 31.0f) == 31.0f);
  KCD2S_CHECK(ruled_priced_health(priced(2, 1, 1, 2), 12.0f, 12.0f, 31.0f) == 62.0f);
  // Past the game's cap of a blow (already in the re-price): the health scale goes past it.
  KCD2S_CHECK(ruled_priced_health(priced(5, 1, 1, 1.5f), 120.0f, 120.0f, 200.0f) == 300.0f);
}

KCD2S_TEST(priced_hits, a_refused_or_lighter_ruling_never_keeps_the_game_knock_out) {
  // The game's lethal price took 0 health and knocked out (held back).
  KCD2S_CHECK(!ruled_knockout(HitVerdict{1, HitRuling::Refused, 0}, true, 3.49f, 3.49f, 0, 2));
  KCD2S_CHECK(ruled_knockout(HitVerdict{1, HitRuling::Native, 0}, true, 3.49f, 3.49f, 0, 2));
  KCD2S_CHECK(!ruled_knockout(HitVerdict{1, HitRuling::Set, 1.0f}, true, 3.49f, 3.49f, 1, 2));
  KCD2S_CHECK(ruled_knockout(HitVerdict{1, HitRuling::Set, 5.0f}, true, 3.49f, 3.49f, 5, 2));
  KCD2S_CHECK(!ruled_knockout(HitVerdict{1, HitRuling::Scaled, 0.5f}, true, 3.49f, 3.49f, 0, 2));
  KCD2S_CHECK(ruled_knockout(HitVerdict{1, HitRuling::Scaled, 2.0f}, true, 3.49f, 3.49f, 0, 2));
  KCD2S_CHECK(ruled_knockout(priced(2, 1, 1, 1), true, 3.49f, 9.93f, 0, 2));
  KCD2S_CHECK(!ruled_knockout(priced(0.5f, 1, 1, 1), true, 3.49f, 1.0f, 0, 2));
}

KCD2S_TEST(priced_hits, a_ruling_that_takes_all_the_health_left_knocks_out) {
  KCD2S_CHECK(ruled_knockout(HitVerdict{1, HitRuling::Set, 200.0f}, false, 3.49f, 3.49f, 200, 100));
  KCD2S_CHECK(
      !ruled_knockout(HitVerdict{1, HitRuling::Native, 0}, false, 3.49f, 3.49f, 3.49f, 100));
  KCD2S_CHECK(!ruled_knockout(HitVerdict{1, HitRuling::Refused, 0}, false, 0, 0, 0, 0));
}

KCD2S_TEST(priced_hits, a_blow_the_game_lowered_after_its_price_takes_no_more_re_priced) {
  // A non-lethal blow capped at 7 of a price of 12: a heavier re-price is capped the same.
  KCD2S_CHECK(ruled_priced_health(priced(2, 1, 1, 1), 7.0f, 12.0f, 31.0f) == 7.0f);
  KCD2S_CHECK(ruled_priced_health(priced(0.5f, 1, 1, 1), 7.0f, 12.0f, 5.0f) == 5.0f);
  // A blow that took nothing in the end takes nothing.
  KCD2S_CHECK(ruled_priced_health(priced(2, 1, 1, 1), 0.0f, 12.0f, 31.0f) == 0.0f);
  // A blow the price did not stop: the re-price, with what came on top of the price.
  KCD2S_CHECK(ruled_priced_health(priced(2, 1, 1, 1), 14.0f, 12.0f, 31.0f) == 33.0f);
}
