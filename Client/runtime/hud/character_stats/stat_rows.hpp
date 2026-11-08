// What a client resource sets on the inventory's character panel (Hud.setCharacterStats): the
// game's stats it hides, and the label and value it shows in a stat's place instead of the game's.
#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace kcds::runtime::hud::character_stats {

constexpr std::size_t kMostStats = 64;
constexpr std::size_t kMostLabelBytes = 64;
constexpr int kMostValue = 9999;

// A stat's place with a resource's label and value.
struct StatRow {
  std::string stat;
  std::string label;
  int value = 0;
};

struct StatRows {
  // False while the game's own panel shows (nothing set, or set back to null).
  bool active = false;
  std::vector<std::string> hidden;
  std::vector<StatRow> shown;
};

// The stat name of a game row's id, which the game writes as "<number>:<name>".
inline std::string_view stat_name(std::string_view id) {
  const std::size_t colon = id.find(':');
  if (colon == std::string_view::npos) {
    return id;
  }
  return id.substr(colon + 1);
}

// Whether the game's row of this id is hidden.
inline bool is_hidden(const StatRows& rows, std::string_view id) {
  const std::string_view name = stat_name(id);
  return std::any_of(rows.hidden.begin(), rows.hidden.end(),
                     [name](const std::string& hidden) { return hidden == name; });
}

// The resource's row for the game's row of this id, or nullptr.
inline const StatRow* shown_for(const StatRows& rows, std::string_view id) {
  const std::string_view name = stat_name(id);
  for (const StatRow& row : rows.shown) {
    if (row.stat == name) {
      return &row;
    }
  }
  return nullptr;
}

// A label as the panel takes it: at most kMostLabelBytes, cut before a UTF-8 sequence that does
// not fit.
inline std::string fitted_label(std::string_view label) {
  if (label.size() <= kMostLabelBytes) {
    return std::string(label);
  }
  std::size_t end = kMostLabelBytes;
  while (end > 0 && (static_cast<unsigned char>(label[end]) & 0xC0) == 0x80) {
    --end;
  }
  return std::string(label.substr(0, end));
}

inline int fitted_value(double value) {
  if (!(value > -kMostValue)) {
    return -kMostValue;
  }
  if (!(value < kMostValue)) {
    return kMostValue;
  }
  return static_cast<int>(value);
}

}  // namespace kcds::runtime::hud::character_stats
