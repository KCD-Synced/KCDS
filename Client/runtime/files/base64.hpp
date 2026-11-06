// Base64 text (as the resource runtime's btoa writes it) to bytes, for files a resource sends the
// game client in messages (NativeUI's files, custom maps).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kcds::runtime::files {

// Appends the bytes of `text` to `out`; false at a character that is not base64 (padding ends it).
inline bool decode_base64(const std::string& text, std::vector<std::uint8_t>& out) {
  auto value = [](char character) -> int {
    if (character >= 'A' && character <= 'Z') {
      return character - 'A';
    }
    if (character >= 'a' && character <= 'z') {
      return character - 'a' + 26;
    }
    if (character >= '0' && character <= '9') {
      return character - '0' + 52;
    }
    if (character == '+') {
      return 62;
    }
    return character == '/' ? 63 : -1;
  };
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const char character : text) {
    if (character == '=') {
      break;
    }
    const int digit = value(character);
    if (digit < 0) {
      return false;
    }
    buffer = (buffer << 6) | static_cast<std::uint32_t>(digit);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xff));
    }
  }
  return true;
}

}  // namespace kcds::runtime::files
