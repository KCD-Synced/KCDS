// What the server says about a player's nameplate: whether others see
// it, whether it carries the health bar, its colour and a text drawn instead of the nickname. The
// server keeps it in the NetworkPlayer field "nameplate" (Player.setNameplateVisible,
// setNameplateHealthVisible, setNameplateColor, setNameplateText), so every client draws the same
// tag;
// "" is the default: shown, with the health bar, opaque white, the nickname.
//
// The field is "<visible 0|1><health 0|1>;<colour, 8 hex digits AARRGGBB>;<text>", the text at
// most kMaxNameplateTextBytes UTF-8 bytes with no line breaks. A tag whose name is hidden (only the
// health bar, Player.setNameplateMode('health')) has a third flag, "<visible><health><name 0>;...".
// A tag with a title above the name (Player.setNameplateTitle, a role such as "Town guard") ends
// with a line break, the title's colour (8 hex digits AARRGGBB, 00000000 for the plate's own gold),
// ';', its size (decimal pixels at 1080 high, kMinNameplateTitleSize to kMaxNameplateTitleSize, 0
// for the plate's own), ';' and the title, at most kMaxNameplateTextBytes too; neither text has a
// line break.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

namespace kcd2s::integration {

inline constexpr std::size_t kMaxNameplateTextBytes = 64;
inline constexpr std::uint32_t kNameplateWhite = 0xFFFFFFFFu;
inline constexpr unsigned kMinNameplateTitleSize = 8;
inline constexpr unsigned kMaxNameplateTitleSize = 48;

struct NameplateState {
  bool visible = true;
  bool health_visible = true;
  bool name_visible = true;
  std::uint32_t color = kNameplateWhite;
  std::string text;
  std::string title;
  std::uint32_t title_color = 0;  // 0: the plate's own gold
  unsigned title_size = 0;        // pixels at 1080 high; 0: the plate's own

  friend bool operator==(const NameplateState& a, const NameplateState& b) {
    return a.visible == b.visible && a.health_visible == b.health_visible &&
           a.name_visible == b.name_visible && a.color == b.color && a.text == b.text &&
           a.title == b.title && a.title_color == b.title_color && a.title_size == b.title_size;
  }
};

// Eight upper-case hex digits at `at` as AARRGGBB; false for anything else.
inline bool parse_nameplate_color(const std::string& field, std::size_t at, std::uint32_t& color) {
  if (field.size() < at + 8) {
    return false;
  }
  color = 0;
  for (std::size_t i = at; i < at + 8; ++i) {
    const char c = field[i];
    std::uint32_t digit = 0;
    if (c >= '0' && c <= '9') {
      digit = std::uint32_t(c - '0');
    } else if (c >= 'A' && c <= 'F') {
      digit = std::uint32_t(c - 'A' + 10);
    } else {
      return false;
    }
    color = color * 16 + digit;
  }
  return true;
}

inline bool nameplate_text_valid(const std::string& text) {
  if (text.size() > kMaxNameplateTextBytes) {
    return false;
  }
  for (const char c : text) {
    if (c == '\n' || c == '\r' || c == '\0') {
      return false;
    }
  }
  return true;
}

// The field for a state; "" for the default one.
inline std::string nameplate_field(const NameplateState& state) {
  if (state == NameplateState{}) {
    return {};
  }
  char head[16];
  if (state.name_visible) {
    std::snprintf(head, sizeof(head), "%c%c;%08X;", state.visible ? '1' : '0',
                  state.health_visible ? '1' : '0', unsigned(state.color));
  } else {
    std::snprintf(head, sizeof(head), "%c%c0;%08X;", state.visible ? '1' : '0',
                  state.health_visible ? '1' : '0', unsigned(state.color));
  }
  std::string field = head + state.text;
  if (!state.title.empty()) {
    char title_head[24];
    std::snprintf(title_head, sizeof(title_head), "\n%08X;%u;", unsigned(state.title_color),
                  state.title_size);
    field += title_head + state.title;
  }
  return field;
}

// A title size: 0 (the plate's own) or within the bounds.
inline bool nameplate_title_size_valid(unsigned size) {
  return size == 0 || (size >= kMinNameplateTitleSize && size <= kMaxNameplateTitleSize);
}

inline bool parse_nameplate_field(const std::string& field, NameplateState& state) {
  if (field.empty()) {
    state = NameplateState{};
    return true;
  }
  // Two flags, or three with the name's.
  const std::size_t flags = field.size() > 2 && field[2] == ';' ? 2 : 3;
  if (field.size() < flags + 10 || field[flags] != ';' || field[flags + 9] != ';') {
    return false;
  }
  const char visible = field[0];
  const char health = field[1];
  const char name = flags == 3 ? field[2] : '1';
  if ((visible != '0' && visible != '1') || (health != '0' && health != '1') ||
      (name != '0' && name != '1')) {
    return false;
  }
  std::uint32_t color = 0;
  if (!parse_nameplate_color(field, flags + 1, color)) {
    return false;
  }
  std::string text = field.substr(flags + 10);
  std::string title;
  std::uint32_t title_color = 0;
  unsigned title_size = 0;
  const std::size_t line = text.find('\n');
  if (line != std::string::npos) {
    // "<AARRGGBB>;<size>;<title>" after the line break.
    const std::string title_line = text.substr(line + 1);
    text.resize(line);
    if (title_line.size() < 10 || title_line[8] != ';' ||
        !parse_nameplate_color(title_line, 0, title_color)) {
      return false;
    }
    const std::size_t size_end = title_line.find(';', 9);
    if (size_end == std::string::npos || size_end == 9 || size_end > 11) {
      return false;
    }
    for (std::size_t i = 9; i < size_end; ++i) {
      if (title_line[i] < '0' || title_line[i] > '9') {
        return false;
      }
      title_size = title_size * 10 + unsigned(title_line[i] - '0');
    }
    title = title_line.substr(size_end + 1);
    if (!nameplate_title_size_valid(title_size) || title.empty() || !nameplate_text_valid(title)) {
      return false;
    }
  }
  if (!nameplate_text_valid(text)) {
    return false;
  }
  state.visible = visible == '1';
  state.health_visible = health == '1';
  state.name_visible = name == '1';
  state.color = color;
  state.text = std::move(text);
  state.title = std::move(title);
  state.title_color = title_color;
  state.title_size = title_size;
  return true;
}

}  // namespace kcd2s::integration
