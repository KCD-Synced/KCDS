#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "kcd2s/core/bits.hpp"
#include "kcd2s/core/ids.hpp"
namespace kcd2s::integration {
inline constexpr std::uint8_t kChatChannel = 7;
struct ChatSubmission {
  core::EntityId player{};
  std::string text;
};
struct ChatMessage {
  std::string author, text;
  std::uint32_t color{};
};
inline bool valid_chat_text(const std::string& text, std::size_t limit = 1024) {
  if (text.empty() || text.size() > limit)
    return false;
  const auto* p = reinterpret_cast<const unsigned char*>(text.data());
  for (std::size_t i = 0; i < text.size();) {
    const auto c = p[i++];
    if (c < 32 || c == 127)
      return false;
    if (c < 128)
      continue;
    unsigned n = 0;
    std::uint32_t value = 0, min = 0;
    if (c >= 0xc2 && c <= 0xdf) {
      n = 1;
      value = c & 31;
      min = 0x80;
    } else if (c >= 0xe0 && c <= 0xef) {
      n = 2;
      value = c & 15;
      min = 0x800;
    } else if (c >= 0xf0 && c <= 0xf4) {
      n = 3;
      value = c & 7;
      min = 0x10000;
    } else
      return false;
    if (i + n > text.size())
      return false;
    while (n--) {
      const auto b = p[i++];
      if ((b & 0xc0) != 0x80)
        return false;
      value = (value << 6) | (b & 63);
    }
    if (value < min || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
      return false;
  }
  return true;
}
inline void chat_string(core::BitWriter& w, const std::string& text) {
  w.write_bits(text.size(), 16);
  if (!text.empty())
    w.write_bytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}
inline bool chat_string(core::BitReader& r, std::string& text, std::size_t limit) {
  const auto n = r.read_bits(16);
  if (r.overrun() || n > limit || n > r.bits_remaining() / 8)
    return false;
  text.resize(std::size_t(n));
  for (auto& c : text)
    c = char(r.read_bits(8));
  return !r.overrun();
}
inline std::vector<std::uint8_t> encode_chat_submission(std::uint32_t revision,
                                                        const ChatSubmission& e) {
  core::BitWriter w;
  w.write_bits(0x3148434b, 32);
  w.write_bits(1, 8);
  w.write_bits(revision, 32);
  w.write_bits(e.player, 32);
  chat_string(w, e.text);
  return w.bytes();
}
inline bool decode_chat_submission(const std::vector<std::uint8_t>& bytes, std::uint32_t& revision,
                                   ChatSubmission& e) {
  core::BitReader r(bytes);
  if (r.read_bits(32) != 0x3148434b || r.read_bits(8) != 1)
    return false;
  revision = std::uint32_t(r.read_bits(32));
  e.player = std::uint32_t(r.read_bits(32));
  return chat_string(r, e.text, 1024) && !r.overrun() && r.bits_remaining() == 0 &&
         valid_chat_text(e.text);
}
inline std::vector<std::uint8_t> encode_chat_message(std::uint32_t revision, const ChatMessage& e) {
  core::BitWriter w;
  w.write_bits(0x3148434b, 32);
  w.write_bits(2, 8);
  w.write_bits(revision, 32);
  w.write_bits(e.color, 32);
  chat_string(w, e.author);
  chat_string(w, e.text);
  return w.bytes();
}
inline bool decode_chat_message(const std::vector<std::uint8_t>& bytes, std::uint32_t& revision,
                                ChatMessage& e) {
  core::BitReader r(bytes);
  if (r.read_bits(32) != 0x3148434b || r.read_bits(8) != 2)
    return false;
  revision = std::uint32_t(r.read_bits(32));
  e.color = std::uint32_t(r.read_bits(32));
  return chat_string(r, e.author, 96) && chat_string(r, e.text, 2048) && !r.overrun() &&
         r.bits_remaining() == 0 && (e.author.empty() || valid_chat_text(e.author, 96)) &&
         valid_chat_text(e.text, 2048);
}
}  // namespace kcd2s::integration
