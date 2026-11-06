#include "pack_index.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>

namespace kcds::runtime::studio::pack_index {
namespace {

constexpr std::uint32_t kEndRecord = 0x06054b50;
constexpr std::uint32_t kDirectoryRecord = 0x02014b50;
constexpr std::size_t kEndRecordSize = 22;
constexpr std::size_t kDirectoryRecordSize = 46;

std::uint32_t u32(const unsigned char* at) {
  return std::uint32_t(at[0]) | (std::uint32_t(at[1]) << 8) | (std::uint32_t(at[2]) << 16) |
         (std::uint32_t(at[3]) << 24);
}

std::uint16_t u16(const unsigned char* at) {
  return std::uint16_t(at[0] | (at[1] << 8));
}

std::string lower(std::string text) {
  for (char& c : text) {
    c = char(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

}  // namespace

bool find_directory(const std::vector<unsigned char>& tail, unsigned long long tail_start,
                    unsigned long long& offset, unsigned long long& size) {
  if (tail.size() < kEndRecordSize) {
    return false;
  }
  for (std::size_t at = tail.size() - kEndRecordSize + 1; at-- > 0;) {
    if (u32(&tail[at]) != kEndRecord) {
      continue;
    }
    size = u32(&tail[at + 12]);
    offset = u32(&tail[at + 16]);
    return offset + size <= tail_start + at;
  }
  return false;
}

std::vector<std::string> directory_names(const std::vector<unsigned char>& directory) {
  std::vector<std::string> names;
  std::size_t at = 0;
  while (at + kDirectoryRecordSize <= directory.size() && u32(&directory[at]) == kDirectoryRecord) {
    const std::size_t name = u16(&directory[at + 28]);
    const std::size_t extra = u16(&directory[at + 30]);
    const std::size_t comment = u16(&directory[at + 32]);
    if (at + kDirectoryRecordSize + name > directory.size()) {
      break;
    }
    names.emplace_back(reinterpret_cast<const char*>(&directory[at + kDirectoryRecordSize]), name);
    at += kDirectoryRecordSize + name + extra + comment;
  }
  return names;
}

std::vector<std::string> matching(const std::vector<std::string>& paths, const std::string& query) {
  std::vector<std::string> words;
  std::istringstream split(lower(query));
  for (std::string word; split >> word;) {
    words.push_back(word);
  }
  std::vector<std::string> found;
  for (const std::string& path : paths) {
    const std::string low = lower(path);
    const bool all = std::all_of(words.begin(), words.end(), [&](const std::string& word) {
      return low.find(word) != std::string::npos;
    });
    if (all) {
      found.push_back(path);
    }
  }
  return found;
}

}  // namespace kcds::runtime::studio::pack_index
