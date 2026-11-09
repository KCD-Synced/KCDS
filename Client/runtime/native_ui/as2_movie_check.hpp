// Tests only: reads back a movie of as2_script.hpp, checking that it is one frame of ActionScript 2
// whose jumps and functions stay inside their action lists, and collects the texts it pushes.
#ifndef KCDS_RUNTIME_NATIVE_UI_AS2_MOVIE_CHECK_HPP
#define KCDS_RUNTIME_NATIVE_UI_AS2_MOVIE_CHECK_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kcds::runtime::native_ui::as2::check {

using Bytes = std::vector<std::uint8_t>;

inline std::size_t u16(const Bytes& bytes, std::size_t at) {
  return bytes[at] | (bytes[at + 1] << 8);
}

inline std::size_t u32(const Bytes& bytes, std::size_t at) {
  return u16(bytes, at) | (u16(bytes, at + 2) << 16);
}

// The texts of one push's payload; false on a value type the writer does not make.
inline bool push_texts(const Bytes& code, std::size_t payload, std::size_t length,
                       std::vector<std::string>& texts) {
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
  return true;
}

// One action list [begin, end): every action inside it, every jump onto an action of the same
// list, every function body a list of its own.
inline bool walk(const Bytes& code, std::size_t begin, std::size_t end,
                 std::vector<std::string>& texts) {
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
    if (op == 0x96 && !push_texts(code, payload, length, texts)) {
      return false;
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

// The whole movie: its header, an empty rectangle, the frame rate and one frame, then
// FileAttributes, DoAction, ShowFrame and End, and the actions well formed.
inline bool read_movie(const Bytes& movie, std::vector<std::string>& texts) {
  if (movie.size() < 16 || movie[0] != 'F' || movie[1] != 'W' || movie[2] != 'S' || movie[3] != 8 ||
      u32(movie, 4) != movie.size()) {
    return false;
  }
  std::size_t at = 8 + 1 + 2;
  if (u16(movie, at) != 1) {
    return false;
  }
  at += 2;
  std::vector<unsigned> codes;
  std::size_t action_begin = 0;
  std::size_t action_end = 0;
  while (at + 6 <= movie.size()) {
    const unsigned code = unsigned(u16(movie, at) >> 6);
    const std::size_t length = u32(movie, at + 2);
    codes.push_back(code);
    if (code == 12) {
      action_begin = at + 6;
      action_end = at + 6 + length;
    }
    at += 6 + length;
  }
  return at == movie.size() && codes == std::vector<unsigned>({69, 12, 1, 0}) &&
         action_end > action_begin && movie[action_end - 1] == 0 &&
         walk(movie, action_begin, action_end - 1, texts);
}

}  // namespace kcds::runtime::native_ui::as2::check

#endif
