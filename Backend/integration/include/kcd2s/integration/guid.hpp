#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

// A GUID as the game keeps it (perks, buffs): the standard layout, data1, data2 and data3
// little-endian, data4 byte by byte.

namespace kcd2s::integration {

using Guid = std::array<std::uint8_t, 16>;

namespace detail {

// The byte each text digit pair comes from.
inline constexpr std::array<int, 16> kGuidTextOrder = {3, 2, 1,  0,  5,  4,  7,  6,
                                                       8, 9, 10, 11, 12, 13, 14, 15};

}  // namespace detail

// The GUID's text as the game's tables write it: lower-case, 8-4-4-4-12.
inline std::string guid_text(const Guid& id) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text;
  for (std::size_t i = 0; i < detail::kGuidTextOrder.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      text += '-';
    }
    const std::uint8_t byte = id[detail::kGuidTextOrder[i]];
    text += kDigits[byte >> 4];
    text += kDigits[byte & 15];
  }
  return text;
}

// The GUID of its text (guid_text's form, either case); false for anything else.
inline bool parse_guid(const std::string& text, Guid& id) {
  if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' ||
      text[23] != '-') {
    return false;
  }
  const auto digit = [](char c) {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  };
  Guid parsed{};
  std::size_t at = 0;
  for (std::size_t i = 0; i < detail::kGuidTextOrder.size(); ++i) {
    if (text[at] == '-') {
      ++at;
    }
    const int high = digit(text[at]);
    const int low = digit(text[at + 1]);
    if (high < 0 || low < 0) {
      return false;
    }
    parsed[detail::kGuidTextOrder[i]] = std::uint8_t(high << 4 | low);
    at += 2;
  }
  id = parsed;
  return true;
}

}  // namespace kcd2s::integration
