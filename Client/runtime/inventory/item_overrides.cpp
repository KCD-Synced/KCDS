#include "item_overrides.hpp"

#include "equipped_weight.hpp"

#include <cstdint>
#include <cstring>
#include <map>
#include <optional>

#include "item_types.hpp"
#include "max_column.hpp"

namespace kcds::runtime::inventory {
namespace {

namespace integration = kcd2s::integration;

// An item class's weight and price (see item_overrides.hpp).
constexpr std::size_t kClassWeight = 0x58;
constexpr std::size_t kClassPrice = 0x5C;

// The soul's base whose vtable marks its derived values stale (see item_overrides.hpp).
constexpr std::size_t kSoulStatsBase = 8;
constexpr std::size_t kStatsStaleSlot = 3;

// Where a stat lives in an item class (WHGame 1.5.5, read from the classes of a helmet, a long
// sword, a bow, an apple, a beer and a crossbow bolt against their table rows), and whether the
// game keeps it whole.
// The server sends only the stats an item's category carries.
struct StatField {
  integration::ItemStat stat;
  std::size_t offset;
  bool whole;
};
constexpr StatField kStatFields[] = {
    {integration::ItemStat::kVisibility, 0xB0, false},
    {integration::ItemStat::kConspicuousness, 0xB4, false},
    {integration::ItemStat::kCharisma, 0xB8, false},
    {integration::ItemStat::kArmorDefenseStab, 0xF0, false},
    {integration::ItemStat::kArmorDefenseSlash, 0xF4, false},
    {integration::ItemStat::kArmorDefenseSmash, 0xF8, false},
    {integration::ItemStat::kArmorStrength, 0xFC, true},
    {integration::ItemStat::kArmorNoise, 0x100, false},
    {integration::ItemStat::kArmorDurability, 0x104, true},
    {integration::ItemStat::kWeaponDefense, 0x110, false},
    {integration::ItemStat::kWeaponDurability, 0x114, true},
    {integration::ItemStat::kWeaponStrength, 0x118, true},
    {integration::ItemStat::kWeaponAgility, 0x11C, true},
    {integration::ItemStat::kWeaponAttack, 0x138, false},
    {integration::ItemStat::kWeaponAttackModStab, 0x13C, false},
    {integration::ItemStat::kWeaponAttackModSlash, 0x140, false},
    {integration::ItemStat::kWeaponAttackModSmash, 0x144, false},
    {integration::ItemStat::kFoodHealth, 0xF0, false},
    {integration::ItemStat::kFoodRefresh, 0xF4, false},
    {integration::ItemStat::kFoodNutrition, 0xF8, false},
    {integration::ItemStat::kFoodAlcohol, 0x100, false},
    {integration::ItemStat::kFoodDecayTime, 0x134, false},
    // Food's ShortTermNutritionBenefitRatio (its reflection).
    {integration::ItemStat::kFoodShortTermNutrition, 0xFC, false},
    {integration::ItemStat::kAmmoAttackStab, 0xF8, false},
    {integration::ItemStat::kAmmoPowerMod, 0x104, false},
    // Ammo's AttackSlash and AttackSmash (its reflection: getters).
    {integration::ItemStat::kAmmoAttackSlash, 0xFC, false},
    {integration::ItemStat::kAmmoAttackSmash, 0x100, false},
};

const StatField* stat_field(std::uint8_t stat) {
  for (const StatField& field : kStatFields) {
    if (std::uint8_t(field.stat) == stat) {
      return &field;
    }
  }
  return nullptr;
}

// What a class held before the server changed it: the four bytes at each offset changed.
struct Original {
  std::uint8_t* item_class = nullptr;
  std::map<std::size_t, std::uint32_t> words;

  // Keeps the class's own word at offset (the first time) and writes the new one.
  void write(std::size_t offset, const void* value) {
    if (words.count(offset) == 0) {
      std::uint32_t word = 0;
      std::memcpy(&word, item_class + offset, sizeof(word));
      words[offset] = word;
    }
    std::memcpy(item_class + offset, value, sizeof(std::uint32_t));
  }
};

std::map<integration::Guid, Original> originals;
std::optional<integration::ItemOverrides> waiting;

void write_stat(Original& original, const integration::ItemStatValue& stat) {
  const StatField* field = stat_field(stat.stat);
  if (!field) {
    return;
  }
  if (field->whole) {
    const auto whole = std::int32_t(stat.value);
    original.write(field->offset, &whole);
  } else {
    original.write(field->offset, &stat.value);
  }
}

// The soul works its derived values, its inventory's weight among them, out again.
void refresh_soul(void* soul) {
  if (!soul) {
    return;
  }
  auto* base = static_cast<std::uint8_t*>(soul) + kSoulStatsBase;
  void** table = nullptr;
  std::memcpy(&table, base, sizeof(table));
  if (!table) {
    return;
  }
  using MarkStale = void(__fastcall*)(void*);
  reinterpret_cast<MarkStale>(table[kStatsStaleSlot])(base);
}

void restore_all() {
  for (const auto& [item, original] : originals) {
    for (const auto& [offset, word] : original.words) {
      std::memcpy(original.item_class + offset, &word, sizeof(word));
    }
  }
  originals.clear();
}

// Whether the game answers for any class of the set. A class the game's tables do not have
// answers null too, so a set waits only while none answers.
bool classes_known(const integration::ItemOverrides& overrides) {
  if (overrides.items.empty()) {
    return true;
  }
  for (const integration::ItemOverride& item : overrides.items) {
    if (game_item_class(item.item)) {
      return true;
    }
  }
  return false;
}

// Applies a set: false while the game has looked up no class yet.
bool apply(const integration::ItemOverrides& overrides, void* soul, Logger log) {
  if (!classes_known(overrides)) {
    return false;
  }
  const bool had_any = !originals.empty();
  restore_all();
  std::size_t applied = 0;
  std::map<integration::Guid, std::uint32_t> limits;
  for (const integration::ItemOverride& item : overrides.items) {
    if (item.has_max) {
      limits[item.item] = item.max;
    }
    auto* item_class = static_cast<std::uint8_t*>(game_item_class(item.item));
    if (!item_class) {
      continue;
    }
    Original& original = originals[item.item];
    original.item_class = item_class;
    if (item.has_weight) {
      original.write(kClassWeight, &item.weight);
    }
    if (item.has_price) {
      original.write(kClassPrice, &item.price);
    }
    for (const integration::ItemStatValue& stat : item.stats) {
      write_stat(original, stat);
    }
    ++applied;
  }
  set_max_column_limits(std::move(limits), overrides.max_tooltip);
  // The share of worn items' weight changes the soul's weight too.
  const bool share_changed = equipped_weight::set_percent(overrides.equipped_weight_percent);
  if (had_any || applied > 0 || share_changed) {
    refresh_soul(soul);
  }
  if (log) {
    log("INVENTORY item overrides: %zu of %zu classes changed", applied, overrides.items.size());
  }
  return true;
}

}  // namespace

void apply_item_overrides(const integration::ItemOverrides& overrides, void* soul, Logger log) {
  if (apply(overrides, soul, log)) {
    waiting.reset();
  } else {
    waiting = overrides;
  }
}

void keep_item_overrides(void* soul, Logger log) {
  if (waiting && apply(*waiting, soul, log)) {
    waiting.reset();
  }
}

void reset_item_overrides() {
  // The world is gone with its souls; the next one works its values out from the game's own.
  restore_all();
  waiting.reset();
  set_max_column_limits({}, "");
  equipped_weight::set_percent(100);
}

}  // namespace kcds::runtime::inventory
