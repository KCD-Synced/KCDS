#include "kcd2s/bridge/sigscan.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

namespace kcd2s::bridge {

namespace {
int hex_nibble(char c) {
  static constexpr char kDigits[] = "0123456789abcdef";
  const char folded = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
  for (int digit = 0; digit < 16; ++digit) {
    if (kDigits[digit] == folded) {
      return digit;
    }
  }
  return -1;
}
}  // namespace

std::optional<Signature> Signature::parse(std::string_view pattern, bool lenient) {
  Signature sig;
  std::size_t i = 0;
  const std::size_t n = pattern.size();
  while (i < n) {
    if (std::isspace(static_cast<unsigned char>(pattern[i]))) {
      ++i;
      continue;
    }
    if (pattern[i] == '?') {
      sig.bytes_.push_back(SigByte{0, true});
      ++i;
      if (i < n && pattern[i] == '?')
        ++i;  // accept "??" or "?"
      continue;
    }
    const int hi = hex_nibble(pattern[i]);
    if (hi < 0 || i + 1 >= n) {
      if (lenient)
        break;
      return std::nullopt;
    }
    const int lo = hex_nibble(pattern[i + 1]);
    if (lo < 0) {
      if (lenient)
        break;
      return std::nullopt;
    }
    sig.bytes_.push_back(SigByte{static_cast<std::uint8_t>((hi << 4) | lo), false});
    i += 2;
  }
  if (sig.bytes_.empty())
    return std::nullopt;
  return sig;
}

bool Signature::match_at(const std::uint8_t* p, std::size_t avail) const {
  if (avail < bytes_.size())
    return false;
  for (std::size_t k = 0; k < bytes_.size(); ++k) {
    if (bytes_[k].wildcard)
      continue;
    if (p[k] != bytes_[k].value)
      return false;
  }
  return true;
}

namespace {
// Bytes that fill x64 code (REX prefixes, mov/lea/call opcodes, stack offsets, padding): a scan
// that looks for one of them first stops almost everywhere.
bool common_code_byte(std::uint8_t b) {
  switch (b) {
    case 0x00:
    case 0x01:
    case 0x08:
    case 0x0F:
    case 0x10:
    case 0x20:
    case 0x24:
    case 0x28:
    case 0x30:
    case 0x40:
    case 0x41:
    case 0x44:
    case 0x45:
    case 0x48:
    case 0x49:
    case 0x4C:
    case 0x83:
    case 0x85:
    case 0x89:
    case 0x8B:
    case 0x8D:
    case 0xC0:
    case 0xC3:
    case 0xCC:
    case 0xE8:
    case 0xFF:
      return true;
    default:
      return false;
  }
}

// The pattern byte the scan looks for first: the first fixed byte that is not common in code, else
// the first fixed byte; knpos for a pattern of wildcards only.
std::size_t anchor_of(const std::vector<SigByte>& bytes) {
  std::size_t fixed = knpos;
  for (std::size_t k = 0; k < bytes.size(); ++k) {
    if (bytes[k].wildcard) {
      continue;
    }
    if (!common_code_byte(bytes[k].value)) {
      return k;
    }
    if (fixed == knpos) {
      fixed = k;
    }
  }
  return fixed;
}
}  // namespace

// memchr finds the anchor byte (vectorised), and only there is the whole pattern compared: a
// module of ~90 MB is scanned in milliseconds instead of a pattern comparison at every byte
// (~320 signatures, three scans each, took 23 s of a join).
std::size_t scan(const std::uint8_t* data, std::size_t len, const Signature& sig) {
  if (sig.empty() || sig.size() > len)
    return knpos;
  const std::size_t last = len - sig.size();
  const auto& bytes = sig.bytes();
  const std::size_t anchor = anchor_of(bytes);
  if (anchor == knpos) {
    return 0;
  }
  const std::uint8_t value = bytes[anchor].value;
  std::size_t i = 0;
  while (i <= last) {
    const void* hit = std::memchr(data + i + anchor, value, last - i + 1);
    if (!hit) {
      return knpos;
    }
    i = static_cast<std::size_t>(static_cast<const std::uint8_t*>(hit) - data) - anchor;
    if (sig.match_at(data + i, len - i)) {
      return i;
    }
    ++i;
  }
  return knpos;
}

std::size_t rip_target(const std::uint8_t* image, std::size_t image_len, std::size_t disp_off,
                       std::size_t next_insn_off) {
  if (disp_off + 4 > image_len)
    return knpos;
  std::int32_t disp =
      static_cast<std::int32_t>(static_cast<std::uint32_t>(image[disp_off]) |
                                (static_cast<std::uint32_t>(image[disp_off + 1]) << 8) |
                                (static_cast<std::uint32_t>(image[disp_off + 2]) << 16) |
                                (static_cast<std::uint32_t>(image[disp_off + 3]) << 24));
  const std::int64_t target = static_cast<std::int64_t>(next_insn_off) + disp;
  if (target < 0 || static_cast<std::size_t>(target) >= image_len)
    return knpos;
  return static_cast<std::size_t>(target);
}

std::size_t rtti_vtable(const std::uint8_t* image, std::size_t image_len, std::uint64_t image_base,
                        std::string_view type_name) {
  if (type_name.empty()) {
    return knpos;
  }
  // The name with its terminating zero, as the type descriptor keeps it.
  std::string name(type_name);
  name.push_back('\0');
  const std::uint8_t* const end = image + image_len;
  const std::uint8_t* found =
      std::search(image, end, reinterpret_cast<const std::uint8_t*>(name.data()),
                  reinterpret_cast<const std::uint8_t*>(name.data()) + name.size());
  if (found == end || found - image < 16) {
    return knpos;
  }
  const auto descriptor = static_cast<std::uint32_t>(found - 16 - image);
  for (std::size_t at = 0; at + 24 <= image_len; at += 4) {
    std::uint32_t fields[6];
    std::memcpy(fields, image + at, sizeof(fields));
    if (fields[0] != 1 || fields[1] != 0 || fields[3] != descriptor || fields[5] != at) {
      continue;
    }
    const std::uint64_t locator = image_base + at;
    for (std::size_t slot = 0; slot + 16 <= image_len; slot += 8) {
      std::uint64_t value = 0;
      std::memcpy(&value, image + slot, sizeof(value));
      if (value == locator) {
        return slot + 8;
      }
    }
  }
  return knpos;
}

}  // namespace kcd2s::bridge
