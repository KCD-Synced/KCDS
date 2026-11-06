#include "max_column_movie.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "kcd2s/testkit.hpp"

namespace {
using kcds::runtime::inventory::max_column_movie;

using Bytes = std::vector<std::uint8_t>;

std::size_t u16(const Bytes& bytes, std::size_t at) {
  return bytes[at] | (bytes[at + 1] << 8);
}

std::size_t u32(const Bytes& bytes, std::size_t at) {
  return u16(bytes, at) | (u16(bytes, at + 2) << 16);
}

// Walks one action list [begin, end): every action inside it, every jump onto an action of the
// same list, every function body a list of its own. The texts the pushes carry are collected.
bool walk(const Bytes& code, std::size_t begin, std::size_t end, std::vector<std::string>& texts) {
  std::vector<std::size_t> starts;
  std::vector<std::size_t> targets;
  std::size_t at = begin;
  while (at < end) {
    starts.push_back(at);
    const std::uint8_t op = code[at];
    if (op < 0x80) {
      at += 1;
      continue;
    }
    if (at + 3 > end) {
      return false;
    }
    const std::size_t length = u16(code, at + 1);
    const std::size_t payload = at + 3;
    if (payload + length > end) {
      return false;
    }
    if (op == 0x99 || op == 0x9D) {
      const auto offset = std::int16_t(u16(code, payload));
      targets.push_back(std::size_t(std::ptrdiff_t(payload + 2) + offset));
    }
    if (op == 0x96) {
      for (std::size_t p = payload; p < payload + length;) {
        const std::uint8_t type = code[p++];
        if (type == 0) {
          std::string text;
          while (p < payload + length && code[p] != 0) {
            text.push_back(char(code[p++]));
          }
          ++p;
          texts.push_back(text);
        } else if (type == 7) {
          p += 4;
        } else if (type == 5) {
          p += 1;
        } else if (type != 3) {
          return false;
        }
      }
    }
    at = payload + length;
    if (op == 0x9B) {
      const std::size_t size = u16(code, at - 2);
      if (at + size > end || !walk(code, at, at + size, texts)) {
        return false;
      }
      at += size;
    }
  }
  starts.push_back(end);
  for (const std::size_t target : targets) {
    bool found = false;
    for (const std::size_t start : starts) {
      found = found || start == target;
    }
    if (!found) {
      return false;
    }
  }
  return at == end;
}

bool has(const std::vector<std::string>& texts, const char* text) {
  for (const std::string& each : texts) {
    if (each == text) {
      return true;
    }
  }
  return false;
}
}  // namespace

KCD2S_TEST(max_column_movie, the_movie_is_one_frame_of_actionscript_2) {
  const Bytes movie = max_column_movie();
  KCD2S_CHECK(movie.size() > 64);
  KCD2S_CHECK(movie[0] == 'F' && movie[1] == 'W' && movie[2] == 'S' && movie[3] == 8);
  KCD2S_CHECK_EQ(u32(movie, 4), movie.size());
  // An empty rectangle, the frame rate and one frame, then FileAttributes, DoAction, ShowFrame
  // and End.
  std::size_t at = 8 + 1 + 2;
  KCD2S_CHECK_EQ(u16(movie, at), 1u);
  at += 2;
  std::vector<unsigned> codes;
  std::size_t action_begin = 0;
  std::size_t action_end = 0;
  while (at + 6 <= movie.size()) {
    const std::size_t header = u16(movie, at);
    const unsigned code = unsigned(header >> 6);
    const std::size_t length = u32(movie, at + 2);
    codes.push_back(code);
    if (code == 12) {
      action_begin = at + 6;
      action_end = at + 6 + length;
    }
    at += 6 + length;
  }
  KCD2S_CHECK_EQ(at, movie.size());
  KCD2S_CHECK(codes == std::vector<unsigned>({69, 12, 1, 0}));
  // The actions end with their end marker; every jump and function stays inside.
  KCD2S_CHECK(action_end > action_begin && movie[action_end - 1] == 0);
  std::vector<std::string> texts;
  KCD2S_CHECK(walk(movie, action_begin, action_end - 1, texts));
  for (const char* name : {"setLimits", "cell", "tint", "header", "headerOf", "drawIcon", "ready",
                           "kcds_tip_max_carry", "m_setDataFunc", "colorizeName"}) {
    KCD2S_CHECK(has(texts, name));
  }
  // KCDS_MAX_COLUMN_MOVIE=<file> keeps the movie for a decompiler.
  if (const char* path = std::getenv("KCDS_MAX_COLUMN_MOVIE")) {
    if (std::FILE* file = std::fopen(path, "wb")) {
      std::fwrite(movie.data(), 1, movie.size(), file);
      std::fclose(file);
    }
  }
}
