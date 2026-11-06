#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "kcd2s/core/bits.hpp"
#include "kcd2s/integration/guid.hpp"
#include "kcd2s/integration/inventory.hpp"

// What the server changes of the game's own items: an item class's weight and price
// and the most units of it a player may hold, as the server's item config (item-config.json in
// the resources folder) or a resource sets them.
//   ITEM_OVERRIDES:    server -> every client, the whole set: for a client that came in, and to
//                      everyone on each change. A class the set no longer names goes back to the
//                      game's own values. The set carries the tooltip of the inventory's max
//                      column in the player's language.
//   ITEM_LIMIT_NOTICE: server -> one client: the player would take more of a class than its
//                      limit; the text is shown as the game's own notification.
// On the control channel (0).

namespace kcd2s::integration {

inline constexpr std::uint8_t kItemOverridesTag = 131;
inline constexpr std::uint8_t kItemLimitNoticeTag = 132;
inline constexpr std::size_t kMaxItemOverrides = 8192;
// The heaviest weight, the dearest price and the highest limit an override may give.
inline constexpr float kMaxItemOverrideWeight = 100000.0f;
inline constexpr std::uint32_t kMaxItemOverridePrice = 100000000;
inline constexpr std::uint32_t kMaxItemOverrideLimit = 1000000;
// The longest max column tooltip and the longest notice, in bytes of UTF-8.
inline constexpr std::size_t kMaxItemLimitTooltipBytes = 128;
inline constexpr std::size_t kMaxItemLimitNoticeBytes = 256;
// The most stats one override changes, and the largest magnitude a stat takes.
inline constexpr std::size_t kMaxItemOverrideStats = 32;
inline constexpr float kMaxItemStatValue = 1000000.0f;

// The stats an override may change (the server checks which an item's category carries:
// Backend/scripting/runtimes/js/inventory/item_stats.cjs; the client knows where each one lives
// in an item class).
enum class ItemStat : std::uint8_t {
  kVisibility = 1,
  kConspicuousness = 2,
  kCharisma = 3,
  kArmorDefenseStab = 10,
  kArmorDefenseSlash = 11,
  kArmorDefenseSmash = 12,
  kArmorStrength = 13,
  kArmorNoise = 14,
  kArmorDurability = 15,
  kWeaponAttack = 20,
  kWeaponAttackModStab = 21,
  kWeaponAttackModSlash = 22,
  kWeaponAttackModSmash = 23,
  kWeaponDefense = 24,
  kWeaponDurability = 25,
  kWeaponStrength = 26,
  kWeaponAgility = 27,
  kFoodHealth = 30,
  kFoodRefresh = 31,
  kFoodNutrition = 32,
  kFoodAlcohol = 33,
  kFoodDecayTime = 34,
  kFoodShortTermNutrition = 35,
  kAmmoAttackStab = 40,
  kAmmoPowerMod = 41,
  kAmmoAttackSlash = 42,
  kAmmoAttackSmash = 43,
};

struct ItemStatValue {
  std::uint8_t stat = 0;
  float value = 0.0f;

  friend bool operator==(const ItemStatValue& a, const ItemStatValue& b) {
    return a.stat == b.stat && a.value == b.value;
  }
};

struct ItemOverride {
  Guid item{};
  bool has_weight = false;
  float weight = 0.0f;
  bool has_price = false;
  std::uint32_t price = 0;
  bool has_max = false;
  std::uint32_t max = 0;
  std::vector<ItemStatValue> stats;

  friend bool operator==(const ItemOverride& a, const ItemOverride& b) {
    return a.item == b.item && a.has_weight == b.has_weight && a.weight == b.weight &&
           a.has_price == b.has_price && a.price == b.price && a.has_max == b.has_max &&
           a.max == b.max && a.stats == b.stats;
  }
};

struct ItemOverrides {
  std::vector<ItemOverride> items;
  std::string max_tooltip;
  // How much of what the player wears and holds equipped counts toward their load, in percent
  // 100 the game's own.
  std::uint8_t equipped_weight_percent = 100;

  friend bool operator==(const ItemOverrides& a, const ItemOverrides& b) {
    return a.items == b.items && a.max_tooltip == b.max_tooltip &&
           a.equipped_weight_percent == b.equipped_weight_percent;
  }
};

struct ItemLimitNotice {
  std::string text;
  // In the middle of the screen, not among the game's notifications.
  bool center = false;

  friend bool operator==(const ItemLimitNotice& a, const ItemLimitNotice& b) {
    return a.text == b.text && a.center == b.center;
  }
};

namespace item_overrides_detail {

inline bool weight_valid(float weight) {
  return std::isfinite(weight) && weight >= 0.0f && weight <= kMaxItemOverrideWeight;
}

inline bool max_valid(std::uint64_t max) {
  return max >= 1 && max <= kMaxItemOverrideLimit;
}

inline bool stat_valid(const ItemStatValue& stat) {
  return stat.stat != 0 && std::isfinite(stat.value) && std::fabs(stat.value) <= kMaxItemStatValue;
}

inline void write_float(core::BitWriter& w, float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  w.write_bits(bits, 32);
}

inline float read_float(core::BitReader& r) {
  const auto bits = std::uint32_t(r.read_bits(32));
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

inline void write_text(core::BitWriter& w, const std::string& text) {
  w.write_varuint(text.size());
  w.write_bytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

inline bool read_text(core::BitReader& r, std::size_t limit, std::string& text) {
  const std::uint64_t size = r.read_varuint();
  if (r.overrun() || size > limit) {
    return false;
  }
  std::string read(std::size_t(size), '\0');
  for (char& c : read) {
    c = char(r.read_bits(8));
  }
  if (r.overrun()) {
    return false;
  }
  text = std::move(read);
  return true;
}

}  // namespace item_overrides_detail

// ITEM_OVERRIDES: [tag][varuint count][each: 16 bytes GUID, u8 flags (1 weight, 2 price, 4 max,
// 8 stats), u32 weight bits when given, varuint price when given, varuint max when given, when
// stats: varuint count and each u8 stat, u32 value bits][varuint tooltip size][tooltip bytes].
// Empty for a set that cannot travel.
inline std::vector<std::uint8_t> encode_item_overrides(const ItemOverrides& overrides) {
  if (overrides.items.size() > kMaxItemOverrides ||
      overrides.max_tooltip.size() > kMaxItemLimitTooltipBytes ||
      overrides.equipped_weight_percent > 100) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kItemOverridesTag, 8);
  w.write_varuint(overrides.items.size());
  for (const ItemOverride& item : overrides.items) {
    if ((!item.has_weight && !item.has_price && !item.has_max && item.stats.empty()) ||
        (item.has_weight && !item_overrides_detail::weight_valid(item.weight)) ||
        (item.has_price && item.price > kMaxItemOverridePrice) ||
        (item.has_max && !item_overrides_detail::max_valid(item.max)) ||
        item.stats.size() > kMaxItemOverrideStats) {
      return {};
    }
    for (const ItemStatValue& stat : item.stats) {
      if (!item_overrides_detail::stat_valid(stat)) {
        return {};
      }
    }
    w.write_bytes(item.item.data(), item.item.size());
    const int flags = (item.has_weight ? 1 : 0) | (item.has_price ? 2 : 0) |
                      (item.has_max ? 4 : 0) | (item.stats.empty() ? 0 : 8);
    w.write_bits(flags, 8);
    if (item.has_weight) {
      item_overrides_detail::write_float(w, item.weight);
    }
    if (item.has_price) {
      w.write_varuint(item.price);
    }
    if (item.has_max) {
      w.write_varuint(item.max);
    }
    if (!item.stats.empty()) {
      w.write_varuint(item.stats.size());
      for (const ItemStatValue& stat : item.stats) {
        w.write_bits(stat.stat, 8);
        item_overrides_detail::write_float(w, stat.value);
      }
    }
  }
  item_overrides_detail::write_text(w, overrides.max_tooltip);
  w.write_bits(overrides.equipped_weight_percent, 8);
  return w.bytes();
}

inline bool decode_item_overrides(const std::vector<std::uint8_t>& p, ItemOverrides& overrides) {
  if (p.size() < 2 || p[0] != kItemOverridesTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  const std::uint64_t count = r.read_varuint();
  if (r.overrun() || count > kMaxItemOverrides) {
    return false;
  }
  ItemOverrides decoded;
  decoded.items.resize(count);
  for (ItemOverride& item : decoded.items) {
    for (std::uint8_t& byte : item.item) {
      byte = std::uint8_t(r.read_bits(8));
    }
    const std::uint64_t flags = r.read_bits(8);
    if (r.overrun() || flags == 0 || flags > 15) {
      return false;
    }
    item.has_weight = (flags & 1) != 0;
    item.has_price = (flags & 2) != 0;
    item.has_max = (flags & 4) != 0;
    if (item.has_weight) {
      item.weight = item_overrides_detail::read_float(r);
      if (!item_overrides_detail::weight_valid(item.weight)) {
        return false;
      }
    }
    if (item.has_price) {
      const std::uint64_t price = r.read_varuint();
      if (price > kMaxItemOverridePrice) {
        return false;
      }
      item.price = std::uint32_t(price);
    }
    if (item.has_max) {
      const std::uint64_t max = r.read_varuint();
      if (!item_overrides_detail::max_valid(max)) {
        return false;
      }
      item.max = std::uint32_t(max);
    }
    if ((flags & 8) != 0) {
      const std::uint64_t stats = r.read_varuint();
      if (r.overrun() || stats == 0 || stats > kMaxItemOverrideStats) {
        return false;
      }
      item.stats.resize(stats);
      for (ItemStatValue& stat : item.stats) {
        stat.stat = std::uint8_t(r.read_bits(8));
        stat.value = item_overrides_detail::read_float(r);
        if (r.overrun() || !item_overrides_detail::stat_valid(stat)) {
          return false;
        }
      }
    }
  }
  if (r.overrun() ||
      !item_overrides_detail::read_text(r, kMaxItemLimitTooltipBytes, decoded.max_tooltip)) {
    return false;
  }
  decoded.equipped_weight_percent = std::uint8_t(r.read_bits(8));
  if (r.overrun() || decoded.equipped_weight_percent > 100 || !inventory_detail::read_whole(r)) {
    return false;
  }
  overrides = std::move(decoded);
  return true;
}

// ITEM_LIMIT_NOTICE: [tag][varuint size][text bytes][u8 center]. Empty for a text that cannot
// travel.
inline std::vector<std::uint8_t> encode_item_limit_notice(const ItemLimitNotice& notice) {
  if (notice.text.empty() || notice.text.size() > kMaxItemLimitNoticeBytes) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kItemLimitNoticeTag, 8);
  item_overrides_detail::write_text(w, notice.text);
  w.write_bits(notice.center ? 1 : 0, 8);
  return w.bytes();
}

inline bool decode_item_limit_notice(const std::vector<std::uint8_t>& p, ItemLimitNotice& notice) {
  if (p.size() < 2 || p[0] != kItemLimitNoticeTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  ItemLimitNotice decoded;
  if (!item_overrides_detail::read_text(r, kMaxItemLimitNoticeBytes, decoded.text) ||
      decoded.text.empty()) {
    return false;
  }
  const auto center = r.read_bits(8);
  if (center > 1 || !inventory_detail::read_whole(r)) {
    return false;
  }
  decoded.center = center == 1;
  notice = std::move(decoded);
  return true;
}

}  // namespace kcd2s::integration
