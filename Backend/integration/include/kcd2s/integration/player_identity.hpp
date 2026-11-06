#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// A player's name, on the control channel (0); tags 1-13 are taken by control.hpp,
// world_config.hpp, knockout.hpp and appearance.hpp.
//   NICKNAME : the name the player's own client connects under (client -> server), accepted once
//              per connection. The server cleans it (clean_nickname) and keeps it in the
//              NetworkPlayer field "nickname", so every client and the resources read the same
//              name; a player without one is "Player <id>".
namespace kcd2s::integration {
inline constexpr std::uint8_t kNicknameTag = 14;
// What a client may send, in bytes, and what a name keeps, in characters.
inline constexpr std::size_t kNicknameMaxBytes = 128;
inline constexpr std::size_t kNicknameMaxCharacters = 32;

inline std::vector<std::uint8_t> encode_nickname(const std::string& name) {
  if (name.empty() || name.size() > kNicknameMaxBytes) {
    return {};
  }
  std::vector<std::uint8_t> p{kNicknameTag, std::uint8_t(name.size())};
  p.insert(p.end(), name.begin(), name.end());
  return p;
}

inline bool decode_nickname(const std::vector<std::uint8_t>& p, std::string& name) {
  if (p.size() < 3 || p[0] != kNicknameTag || p[1] > kNicknameMaxBytes ||
      p.size() != std::size_t(2 + p[1])) {
    return false;
  }
  name.assign(p.begin() + 2, p.end());
  return true;
}

namespace detail {

// The next UTF-8 code point at text[at]; false for an invalid or overlong sequence.
inline bool next_code_point(const std::string& text, std::size_t& at, std::uint32_t& code) {
  const auto byte = [&](std::size_t i) { return static_cast<std::uint8_t>(text[i]); };
  const std::uint8_t first = byte(at);
  std::size_t length = 0;
  std::uint32_t minimum = 0;
  if (first < 0x80) {
    code = first;
    at += 1;
    return true;
  }
  if ((first & 0xE0) == 0xC0) {
    length = 2;
    code = first & 0x1F;
    minimum = 0x80;
  } else if ((first & 0xF0) == 0xE0) {
    length = 3;
    code = first & 0x0F;
    minimum = 0x800;
  } else if ((first & 0xF8) == 0xF0) {
    length = 4;
    code = first & 0x07;
    minimum = 0x10000;
  } else {
    return false;
  }
  if (at + length > text.size()) {
    return false;
  }
  for (std::size_t i = 1; i < length; ++i) {
    if ((byte(at + i) & 0xC0) != 0x80) {
      return false;
    }
    code = (code << 6) | (byte(at + i) & 0x3F);
  }
  if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
    return false;
  }
  at += length;
  return true;
}

inline bool is_control(std::uint32_t code) {
  return code < 0x20 || (code >= 0x7F && code <= 0x9F) || (code >= 0x200B && code <= 0x200F) ||
         (code >= 0x202A && code <= 0x202E) || (code >= 0x2066 && code <= 0x2069) || code == 0xFEFF;
}

}  // namespace detail

// The name a client sent as players see it: valid UTF-8 only (else ""), control and direction
// characters removed, runs of spaces made one, no space at either end, at most
// kNicknameMaxCharacters characters; "" when nothing remains.
inline std::string clean_nickname(const std::string& raw) {
  std::string name;
  std::size_t characters = 0;
  bool space = false;
  std::size_t at = 0;
  while (at < raw.size()) {
    const std::size_t start = at;
    std::uint32_t code = 0;
    if (!detail::next_code_point(raw, at, code)) {
      return {};
    }
    if (detail::is_control(code)) {
      continue;
    }
    if (code == ' ') {
      space = !name.empty();
      continue;
    }
    const std::size_t needed = space ? 2 : 1;
    if (characters + needed > kNicknameMaxCharacters) {
      break;
    }
    if (space) {
      name += ' ';
      ++characters;
      space = false;
    }
    name.append(raw, start, at - start);
    ++characters;
  }
  return name;
}

}  // namespace kcd2s::integration
