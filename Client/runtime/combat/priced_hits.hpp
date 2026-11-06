#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "kcd2s/integration/damage.hpp"

// A melee blow from another player as the local player's game priced it, and its health once
// the server ruled. The game prices a blow in its combat soul's hit:
// a 352-byte calculator (built) holds the blow's event at + 0, the victim's soul at
// + 8, the attacker's at + 16, the items at + 32 and + 48 as handles, the kind of blow at + 192
// (1 for melee), the blow's health effect at + 208 (C_HealthValueEffect: the health at + 28 as
// a negative amount, the body subpart at + 48), what the victim's block put up at + 296 (-1 when
// nothing blocked it) and a perfect block at + 146. Its pricing step takes a price
// input: the attack in each pass (stab, slash, smash) at + 0, the armour where the blow landed
// at + 12, what the block put up at + 24, the body part's coefficient at + 40 and the attacker's
// and the victim's stat query contexts at + 48 and + 56. It reads the victim's stamina (soul
// + 1804) and writes a 32-byte result: the stamina at + 0, the health at + 4, at + 28 whether
// the block gave way. The cap then limits that result (+ 312 and + 316 of the
// calculator). Nothing in these steps changes the game, so the same calculator can be priced
// again with other numbers.
namespace kcds::runtime::combat {

inline constexpr std::size_t kCalculatorBytes = 352;
inline constexpr std::size_t kPriceInputBytes = 64;
inline constexpr std::size_t kPriceResultBytes = 32;
inline constexpr std::size_t kPriceContextBytes = 40;

inline constexpr std::size_t kCalculatorEvent = 0;
inline constexpr std::size_t kCalculatorVictim = 8;
inline constexpr std::size_t kCalculatorAttacker = 16;
inline constexpr std::size_t kCalculatorPerfectBlock = 146;
inline constexpr std::size_t kCalculatorKind = 192;
inline constexpr std::uint32_t kMeleeCalculator = 1;
inline constexpr std::size_t kCalculatorHealthEffect = 208;
inline constexpr std::size_t kHealthEffectAmount = 28;
inline constexpr std::size_t kHealthEffectSubpart = 48;
inline constexpr std::size_t kCalculatorBlockDefense = 296;
inline constexpr std::size_t kCalculatorResult = 312;
inline constexpr std::size_t kCalculatorHealth = 316;
inline constexpr std::size_t kPriceAttack = 0;
inline constexpr std::size_t kPriceArmor = 12;
inline constexpr std::size_t kPriceBlock = 24;
inline constexpr std::size_t kPriceCoefficient = 40;
inline constexpr std::size_t kPriceAttackerContext = 48;
inline constexpr std::size_t kPriceVictimContext = 56;
inline constexpr std::size_t kResultStamina = 0;
inline constexpr std::size_t kResultBlockBroken = 28;

struct alignas(16) Calculator {
  std::array<std::uint8_t, kCalculatorBytes> bytes{};
};
struct alignas(16) PriceInput {
  std::array<std::uint8_t, kPriceInputBytes> bytes{};
};
struct alignas(16) PriceResult {
  std::array<std::uint8_t, kPriceResultBytes> bytes{};
};

// The numbers playerHit shows for a priced blow, from the calculator and the price input the
// game priced it with, its result and the body subpart its health effect names.
kcd2s::integration::HitPrice hit_price_of(const Calculator& calculator, const PriceInput& input,
                                          const PriceResult& result, std::int32_t subpart);

// The price input with a ruling's modifiers on the attack, the armour and the block of every
// pass.
PriceInput modified_input(const PriceInput& input,
                          const kcd2s::integration::HitModifiers& modifiers);

// The health a ruled priced blow takes, 0 or more. `engine` is what the game's blow took;
// for a Priced ruling `priced` is the game's capped price of the blow and `repriced` its capped
// price with the modifiers. Steps after the price can lower what the blow takes (a non-lethal
// blow's cap, a blow that takes nothing); the re-priced blow takes no more than such a blow did.
float ruled_priced_health(const kcd2s::integration::HitVerdict& verdict, float engine, float priced,
                          float repriced);

// Whether a ruled priced blow knocks the player out. `held` is the game's own knock-out of a
// lethal price (held back for the ruling); `health` the ruled health and `before` the player's
// health before it. A refused blow never does; one taken as the game priced it, scaled no lighter
// or priced again no lighter keeps the game's knock-out; any ruling that takes all the health
// left knocks out.
bool ruled_knockout(const kcd2s::integration::HitVerdict& verdict, bool held, float priced,
                    float repriced, float health, float before);

}  // namespace kcds::runtime::combat
