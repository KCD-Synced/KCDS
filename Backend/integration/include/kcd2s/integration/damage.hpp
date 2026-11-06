#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "kcd2s/core/bits.hpp"

// Health a player's own game took off them, on the control channel (0).
//   PLAYER_DAMAGE : the owner's client -> server, once per blow, fall, collision or scripted hit
//                   its game applied to the player: the health taken, the attacker (a player's
//                   network id, 0 for none or anything else), the game's reason (E_DamageReason:
//                   0 unknown, 1 combat, 2 gunshot, 3 starvation, 4 collision, 5 scripted hit,
//                   6 scripted disintegrate, 7 fall, 8 poisoning, 9 bleeding, 10 self harm) and
//                   the body subpart it landed on (the game's body_subpart table, -1 for none).
//                   The resources hear it as playerDamage.
//   COMBAT_CANCEL : server -> a player's client: end melee combat with that opponent
//                   (Player.stopFight): the client drops its lock on the opponent.
//   PLAYER_HIT    : the hit player's client -> server, before a blow from another player is
//                   taken: the request, the attacker's network id, the health and
//                   stamina the blow takes, the attacker's weapon class and, for a melee blow
//                   the client's game priced, the numbers it priced it with. The client holds
//                   the blow's health until the verdict. The resources hear it as playerHit.
//   HIT_VERDICT   : server -> that client: refuse the blow, or take it as the game works it out,
//                   with its health set to a value, scaled by one, or priced again by the game
//                   with modifiers on its attack, armour, block and health.
//   HIT_RULES     : server -> every client, when its world is ready and on every change: whether
//                   blows from other players wait for a ruling (some resource handles
//                   playerHit). Without one the game works a blow out at once, so that blocks
//                   and parries keep their timing.
namespace kcd2s::integration {

inline constexpr std::uint8_t kPlayerDamageTag = 30;
inline constexpr std::uint8_t kCombatCancelTag = 31;
inline constexpr std::uint8_t kPlayerHitTag = 34;
inline constexpr std::uint8_t kHitVerdictTag = 35;
inline constexpr std::uint8_t kHitRulesTag = 36;
inline constexpr std::uint8_t kDamageReasons = 11;
inline constexpr float kMaxDamage = 100000.0f;

struct PlayerDamage {
  float amount = 0;
  std::uint32_t attacker = 0;
  std::uint8_t reason = 0;
  std::int32_t subpart = -1;

  friend bool operator==(const PlayerDamage& a, const PlayerDamage& b) {
    return a.amount == b.amount && a.attacker == b.attacker && a.reason == b.reason &&
           a.subpart == b.subpart;
  }
};

// PLAYER_DAMAGE: [tag][u32 amount bits][varuint attacker][u8 reason][varint subpart]; empty for
// a report that cannot travel.
inline std::vector<std::uint8_t> encode_player_damage(const PlayerDamage& damage) {
  if (!(damage.amount > 0.0f) || !(damage.amount <= kMaxDamage) ||
      damage.reason >= kDamageReasons || damage.subpart < -1) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kPlayerDamageTag, 8);
  std::uint32_t amount_bits = 0;
  std::memcpy(&amount_bits, &damage.amount, sizeof(amount_bits));
  w.write_bits(amount_bits, 32);
  w.write_varuint(damage.attacker);
  w.write_bits(damage.reason, 8);
  w.write_varint(damage.subpart);
  return w.bytes();
}

inline bool decode_player_damage(const std::vector<std::uint8_t>& p, PlayerDamage& damage) {
  if (p.empty() || p[0] != kPlayerDamageTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  PlayerDamage decoded;
  const auto amount_bits = std::uint32_t(r.read_bits(32));
  std::memcpy(&decoded.amount, &amount_bits, sizeof(amount_bits));
  const std::uint64_t attacker = r.read_varuint();
  const std::uint64_t reason = r.read_bits(8);
  const std::int64_t subpart = r.read_varint();
  if (r.overrun() || r.bits_remaining() != 0 || !(decoded.amount > 0.0f) ||
      !(decoded.amount <= kMaxDamage) || attacker > 0xFFFFFFFFu || reason >= kDamageReasons ||
      subpart < -1 || subpart > 0xFFFF) {
    return false;
  }
  decoded.attacker = std::uint32_t(attacker);
  decoded.reason = std::uint8_t(reason);
  decoded.subpart = std::int32_t(subpart);
  damage = decoded;
  return true;
}

// COMBAT_CANCEL: [tag][varuint the opponent's network id].
inline std::vector<std::uint8_t> encode_combat_cancel(std::uint32_t opponent) {
  if (opponent == 0) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kCombatCancelTag, 8);
  w.write_varuint(opponent);
  return w.bytes();
}

inline bool decode_combat_cancel(const std::vector<std::uint8_t>& p, std::uint32_t& opponent) {
  if (p.empty() || p[0] != kCombatCancelTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  const std::uint64_t id = r.read_varuint();
  if (r.overrun() || r.bits_remaining() != 0 || id == 0 || id > 0xFFFFFFFFu) {
    return false;
  }
  opponent = std::uint32_t(id);
  return true;
}

// The numbers the hit player's game priced a melee blow with: in each pass (stab,
// slash, smash) the attacker's attack and the victim's armour where it landed, what the victim's
// block put up (0 when nothing blocked it), how much the place it landed multiplies the damage
// by, the body subpart it landed on (the game's body_subpart table, -1 for none) and how the
// block went.
struct HitPrice {
  std::array<float, 3> attack{};
  std::array<float, 3> armor{};
  float block_defense = 0;
  float body_coefficient = 0;
  std::int32_t subpart = -1;
  bool blocked = false;
  bool perfect_block = false;
  bool block_broken = false;

  friend bool operator==(const HitPrice& a, const HitPrice& b) = default;
};

// A blow from another player, as the hit player's own game worked it out (PLAYER_HIT).
struct PlayerHitReport {
  std::uint32_t request = 0;
  std::uint32_t attacker = 0;
  float damage = 0;
  float stamina = 0;
  bool armed = false;
  std::array<std::uint8_t, 16> weapon{};  // the weapon's class (armed only)
  // An arrow, bolt or ball: `damage` is the health this game already worked out.
  bool missile = false;
  // A melee blow the game priced: `damage` is the health its pricing took.
  bool priced = false;
  HitPrice price;  // priced only

  friend bool operator==(const PlayerHitReport& a, const PlayerHitReport& b) {
    return a.request == b.request && a.attacker == b.attacker && a.damage == b.damage &&
           a.stamina == b.stamina && a.armed == b.armed && (!a.armed || a.weapon == b.weapon) &&
           a.missile == b.missile && a.priced == b.priced && (!a.priced || a.price == b.price);
  }
};

// The server's ruling on a blow (HIT_VERDICT).
enum class HitRuling : std::uint8_t {
  Refused = 0,
  Native = 1,  // as the game works it out
  Set = 2,     // the health it takes is `value`
  Scaled = 3,  // the health the game works out, times `value`
  Priced = 4,  // priced again by the game with `modifiers` (a priced blow)
};

// What a ruling scales on a priced blow: the attack in every pass, the armour where it landed,
// what the block put up, and the health once the game worked it out (after its cap).
struct HitModifiers {
  float attack = 1;
  float defense = 1;
  float block = 1;
  float health = 1;

  friend bool operator==(const HitModifiers& a, const HitModifiers& b) = default;
};

struct HitVerdict {
  std::uint32_t request = 0;
  HitRuling ruling = HitRuling::Refused;
  float value = 0;
  HitModifiers modifiers;  // Priced only

  friend bool operator==(const HitVerdict& a, const HitVerdict& b) {
    return a.request == b.request && a.ruling == b.ruling && a.value == b.value &&
           (a.ruling != HitRuling::Priced || a.modifiers == b.modifiers);
  }
};

namespace detail {

inline bool valid_amount(float amount) {
  return amount >= 0.0f && amount <= kMaxDamage;
}

inline void write_float(core::BitWriter& w, float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  w.write_bits(bits, 32);
}

inline float read_float(core::BitReader& r) {
  const auto bits = std::uint32_t(r.read_bits(32));
  float value = 0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

}  // namespace detail

// PLAYER_HIT: [tag][varuint request][varuint attacker][u32 damage bits][u32 stamina bits]
// [u8 kinds][16 bytes weapon class, armed only][the price, priced only: u32 bits of the attack
// and the armour in each pass, the block defense and the body coefficient, varint subpart,
// u8 block flags]; empty for a report that cannot travel.
// PLAYER_HIT's kinds byte: armed (a weapon's class follows), a missile, priced (a price follows).
inline constexpr std::uint8_t kHitArmed = 1;
inline constexpr std::uint8_t kHitMissile = 2;
inline constexpr std::uint8_t kHitPriced = 4;
// A price's block flags.
inline constexpr std::uint8_t kPriceBlocked = 1;
inline constexpr std::uint8_t kPricePerfectBlock = 2;
inline constexpr std::uint8_t kPriceBlockBroken = 4;

namespace detail {

inline bool valid_price(const HitPrice& price) {
  for (const float value : price.attack) {
    if (!valid_amount(value)) {
      return false;
    }
  }
  for (const float value : price.armor) {
    if (!valid_amount(value)) {
      return false;
    }
  }
  return valid_amount(price.block_defense) && valid_amount(price.body_coefficient) &&
         price.subpart >= -1 && price.subpart <= 0xFFFF;
}

}  // namespace detail

inline std::vector<std::uint8_t> encode_player_hit(const PlayerHitReport& hit) {
  if (hit.attacker == 0 || !detail::valid_amount(hit.damage) ||
      !detail::valid_amount(hit.stamina) || (hit.priced && hit.missile) ||
      (hit.priced && !detail::valid_price(hit.price))) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kPlayerHitTag, 8);
  w.write_varuint(hit.request);
  w.write_varuint(hit.attacker);
  detail::write_float(w, hit.damage);
  detail::write_float(w, hit.stamina);
  w.write_bits(
      (hit.armed ? kHitArmed : 0) | (hit.missile ? kHitMissile : 0) | (hit.priced ? kHitPriced : 0),
      8);
  if (hit.armed) {
    for (const std::uint8_t byte : hit.weapon) {
      w.write_bits(byte, 8);
    }
  }
  if (hit.priced) {
    for (const float value : hit.price.attack) {
      detail::write_float(w, value);
    }
    for (const float value : hit.price.armor) {
      detail::write_float(w, value);
    }
    detail::write_float(w, hit.price.block_defense);
    detail::write_float(w, hit.price.body_coefficient);
    w.write_varint(hit.price.subpart);
    w.write_bits((hit.price.blocked ? kPriceBlocked : 0) |
                     (hit.price.perfect_block ? kPricePerfectBlock : 0) |
                     (hit.price.block_broken ? kPriceBlockBroken : 0),
                 8);
  }
  return w.bytes();
}

inline bool decode_player_hit(const std::vector<std::uint8_t>& p, PlayerHitReport& hit) {
  if (p.empty() || p[0] != kPlayerHitTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  PlayerHitReport decoded;
  const std::uint64_t request = r.read_varuint();
  const std::uint64_t attacker = r.read_varuint();
  decoded.damage = detail::read_float(r);
  decoded.stamina = detail::read_float(r);
  const auto kinds = r.read_bits(8);
  decoded.missile = (kinds & kHitMissile) != 0;
  if (kinds & kHitArmed) {
    decoded.armed = true;
    for (std::uint8_t& byte : decoded.weapon) {
      byte = std::uint8_t(r.read_bits(8));
    }
  }
  std::uint64_t flags = 0;
  if (kinds & kHitPriced) {
    decoded.priced = true;
    for (float& value : decoded.price.attack) {
      value = detail::read_float(r);
    }
    for (float& value : decoded.price.armor) {
      value = detail::read_float(r);
    }
    decoded.price.block_defense = detail::read_float(r);
    decoded.price.body_coefficient = detail::read_float(r);
    const std::int64_t subpart = r.read_varint();
    decoded.price.subpart = subpart < -1 || subpart > 0xFFFF ? -2 : std::int32_t(subpart);
    flags = r.read_bits(8);
    decoded.price.blocked = (flags & kPriceBlocked) != 0;
    decoded.price.perfect_block = (flags & kPricePerfectBlock) != 0;
    decoded.price.block_broken = (flags & kPriceBlockBroken) != 0;
  }
  if (r.overrun() || r.bits_remaining() != 0 || kinds > (kHitArmed | kHitMissile | kHitPriced) ||
      (decoded.priced && decoded.missile) ||
      (decoded.priced && !detail::valid_price(decoded.price)) ||
      flags > (kPriceBlocked | kPricePerfectBlock | kPriceBlockBroken) || request > 0xFFFFFFFFu ||
      attacker == 0 || attacker > 0xFFFFFFFFu || !detail::valid_amount(decoded.damage) ||
      !detail::valid_amount(decoded.stamina)) {
    return false;
  }
  decoded.request = std::uint32_t(request);
  decoded.attacker = std::uint32_t(attacker);
  hit = decoded;
  return true;
}

namespace detail {

inline bool valid_modifiers(const HitModifiers& modifiers) {
  return valid_amount(modifiers.attack) && valid_amount(modifiers.defense) &&
         valid_amount(modifiers.block) && valid_amount(modifiers.health);
}

}  // namespace detail

// HIT_VERDICT: [tag][varuint request][u8 ruling][u32 value bits][Priced only: u32 bits of the
// attack, defense, block and health modifiers]; empty for one that cannot travel.
inline std::vector<std::uint8_t> encode_hit_verdict(const HitVerdict& verdict) {
  const bool priced = verdict.ruling == HitRuling::Priced;
  if (std::uint8_t(verdict.ruling) > std::uint8_t(HitRuling::Priced) ||
      !detail::valid_amount(verdict.value) ||
      (priced && !detail::valid_modifiers(verdict.modifiers))) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kHitVerdictTag, 8);
  w.write_varuint(verdict.request);
  w.write_bits(std::uint8_t(verdict.ruling), 8);
  detail::write_float(w, verdict.value);
  if (priced) {
    detail::write_float(w, verdict.modifiers.attack);
    detail::write_float(w, verdict.modifiers.defense);
    detail::write_float(w, verdict.modifiers.block);
    detail::write_float(w, verdict.modifiers.health);
  }
  return w.bytes();
}

inline bool decode_hit_verdict(const std::vector<std::uint8_t>& p, HitVerdict& verdict) {
  if (p.empty() || p[0] != kHitVerdictTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  HitVerdict decoded;
  const std::uint64_t request = r.read_varuint();
  const auto ruling = r.read_bits(8);
  decoded.value = detail::read_float(r);
  const bool priced = ruling == std::uint8_t(HitRuling::Priced);
  if (priced) {
    decoded.modifiers.attack = detail::read_float(r);
    decoded.modifiers.defense = detail::read_float(r);
    decoded.modifiers.block = detail::read_float(r);
    decoded.modifiers.health = detail::read_float(r);
  }
  if (r.overrun() || r.bits_remaining() != 0 || ruling > std::uint8_t(HitRuling::Priced) ||
      request > 0xFFFFFFFFu || !detail::valid_amount(decoded.value) ||
      (priced && !detail::valid_modifiers(decoded.modifiers))) {
    return false;
  }
  decoded.request = std::uint32_t(request);
  decoded.ruling = HitRuling(ruling);
  verdict = decoded;
  return true;
}

// HIT_RULES: [tag][u8 1 when blows wait for a ruling, else 0].
inline std::vector<std::uint8_t> encode_hit_rules(bool enabled) {
  return {kHitRulesTag, std::uint8_t(enabled ? 1 : 0)};
}

inline bool decode_hit_rules(const std::vector<std::uint8_t>& p, bool& enabled) {
  if (p.size() != 2 || p[0] != kHitRulesTag || p[1] > 1) {
    return false;
  }
  enabled = p[1] == 1;
  return true;
}

}  // namespace kcd2s::integration
