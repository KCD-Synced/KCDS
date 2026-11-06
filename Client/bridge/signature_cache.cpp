#include "kcd2s/bridge/signature_cache.hpp"

#include <charconv>
#include <cstdio>
#include <utility>

namespace kcd2s::bridge {

namespace {

constexpr std::string_view kHeader = "kcds-signatures 1 ";

// FNV-1a: the file names a signature by this, not by its bytes.
std::uint64_t hash_of(std::string_view text) {
  std::uint64_t hash = 14695981039346656037ull;
  for (const char c : text) {
    hash ^= static_cast<std::uint8_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

template <typename Number>
bool parse_hex(std::string_view text, Number& number) {
  const char* end = text.data() + text.size();
  const auto parsed = std::from_chars(text.data(), end, number, 16);
  return parsed.ec == std::errc{} && parsed.ptr == end;
}

}  // namespace

SignatureCache::SignatureCache(std::string build) : build_(std::move(build)) {
}

void SignatureCache::load(std::string_view saved) {
  std::unordered_map<std::uint64_t, std::size_t> offsets;
  bool first = true;
  while (!saved.empty()) {
    const std::size_t end = saved.find('\n');
    if (end == std::string_view::npos) {
      return;
    }
    const std::string_view line = saved.substr(0, end);
    saved.remove_prefix(end + 1);
    if (first) {
      if (line.substr(0, kHeader.size()) != kHeader || line.substr(kHeader.size()) != build_) {
        return;
      }
      first = false;
      continue;
    }
    const std::size_t space = line.find(' ');
    std::uint64_t hash = 0;
    std::uint64_t offset = 0;
    if (space == std::string_view::npos || !parse_hex(line.substr(0, space), hash) ||
        !parse_hex(line.substr(space + 1), offset)) {
      return;
    }
    offsets[hash] = static_cast<std::size_t>(offset);
  }
  if (!first) {
    offsets_ = std::move(offsets);
    changed_ = false;
  }
}

std::optional<std::size_t> SignatureCache::find(std::string_view pattern) const {
  const auto found = offsets_.find(hash_of(pattern));
  if (found == offsets_.end()) {
    return std::nullopt;
  }
  return found->second;
}

void SignatureCache::remember(std::string_view pattern, std::size_t offset) {
  auto& held = offsets_[hash_of(pattern)];
  if (held != offset || !changed_) {
    changed_ = true;
  }
  held = offset;
}

std::string SignatureCache::serialized() const {
  std::string text(kHeader);
  text += build_;
  text += '\n';
  for (const auto& [hash, offset] : offsets_) {
    char line[64] = {};
    std::snprintf(line, sizeof(line), "%llx %llx\n", static_cast<unsigned long long>(hash),
                  static_cast<unsigned long long>(offset));
    text += line;
  }
  return text;
}

}  // namespace kcd2s::bridge
