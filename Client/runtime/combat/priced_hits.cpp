#include "priced_hits.hpp"

#include <algorithm>
#include <cstring>

namespace kcds::runtime::combat {
namespace {

using kcd2s::integration::HitRuling;
using kcd2s::integration::kMaxDamage;

template <std::size_t N>
float read_float(const std::array<std::uint8_t, N>& bytes, std::size_t offset) {
  float value = 0;
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

template <std::size_t N>
void write_float(std::array<std::uint8_t, N>& bytes, std::size_t offset, float value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

// A number the wire carries: 0 or more, finite, at most kMaxDamage.
float amount(float value) {
  if (!(value >= 0.0f)) {
    return 0.0f;
  }
  return std::min(value, kMaxDamage);
}

std::array<float, 3> passes(const PriceInput& input, std::size_t offset) {
  return {amount(read_float(input.bytes, offset)), amount(read_float(input.bytes, offset + 4)),
          amount(read_float(input.bytes, offset + 8))};
}

void scale_passes(PriceInput& input, std::size_t offset, float scale) {
  for (std::size_t pass = 0; pass < 3; ++pass) {
    const std::size_t at = offset + pass * 4;
    write_float(input.bytes, at, read_float(input.bytes, at) * scale);
  }
}

}  // namespace

kcd2s::integration::HitPrice hit_price_of(const Calculator& calculator, const PriceInput& input,
                                          const PriceResult& result, std::int32_t subpart) {
  kcd2s::integration::HitPrice price;
  price.attack = passes(input, kPriceAttack);
  price.armor = passes(input, kPriceArmor);
  // What the block put up: the same in every pass.
  price.blocked = read_float(calculator.bytes, kCalculatorBlockDefense) > 0.0f;
  price.block_defense = price.blocked ? amount(read_float(input.bytes, kPriceBlock)) : 0.0f;
  price.body_coefficient = amount(read_float(input.bytes, kPriceCoefficient));
  price.subpart = subpart >= -1 && subpart <= 0xFFFF ? subpart : -1;
  price.perfect_block = calculator.bytes[kCalculatorPerfectBlock] != 0;
  price.block_broken = result.bytes[kResultBlockBroken] != 0;
  return price;
}

PriceInput modified_input(const PriceInput& input,
                          const kcd2s::integration::HitModifiers& modifiers) {
  PriceInput modified = input;
  scale_passes(modified, kPriceAttack, modifiers.attack);
  scale_passes(modified, kPriceArmor, modifiers.defense);
  scale_passes(modified, kPriceBlock, modifiers.block);
  return modified;
}

float ruled_priced_health(const kcd2s::integration::HitVerdict& verdict, float engine, float priced,
                          float repriced) {
  // What the game's later steps did to the price: lowered it (capped or zeroed), or not.
  constexpr float kSameHealth = 0.001f;
  engine = amount(engine);
  float health = engine;
  switch (verdict.ruling) {
    case HitRuling::Refused:
      health = 0.0f;
      break;
    case HitRuling::Native:
      break;
    case HitRuling::Set:
      health = verdict.value;
      break;
    case HitRuling::Scaled:
      health = engine * verdict.value;
      break;
    case HitRuling::Priced:
      priced = amount(priced);
      repriced = amount(repriced);
      if (engine + kSameHealth < priced) {
        health = std::min(repriced, engine);
      } else {
        health = repriced + (engine - priced);
      }
      health *= verdict.modifiers.health;
      break;
  }
  return amount(health);
}

bool ruled_knockout(const kcd2s::integration::HitVerdict& verdict, bool held, float priced,
                    float repriced, float health, float before) {
  if (verdict.ruling == HitRuling::Refused) {
    return false;
  }
  if (before > 0.0f && health >= before) {
    return true;
  }
  if (!held) {
    return false;
  }
  switch (verdict.ruling) {
    case HitRuling::Native:
      return true;
    case HitRuling::Scaled:
      return verdict.value >= 1.0f;
    case HitRuling::Priced:
      return amount(repriced) >= amount(priced) && verdict.modifiers.health >= 1.0f;
    default:
      return false;
  }
}

}  // namespace kcds::runtime::combat
