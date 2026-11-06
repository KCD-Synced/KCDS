#pragma once
#include "kcd2s/core/ids.hpp"
#include <cstdint>
#include <optional>
#include <string>
namespace kcd2s::local {
// Atomic owner snapshot of the original RiderSync phase. No native pointers.
struct RiderState {
  core::EntityId horse{};
  std::uint64_t generation{};
  std::uint32_t epoch{};
  std::uint8_t phase{}, active_actions{};
};
inline std::string encode_rider(const RiderState& s) {
  std::string out = "KRD2";
  auto word = [&](std::uint64_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i)
      out.push_back(char(v >> (8 * i)));
  };
  word(s.horse, 4);
  word(s.generation, 8);
  word(s.epoch, 4);
  word(s.phase, 1);
  word(s.active_actions, 1);
  return out;
}
inline std::optional<RiderState> decode_rider(const std::string& data) {
  if (data.size() != 22 || data.compare(0, 4, "KRD2") != 0)
    return std::nullopt;
  unsigned pos = 4;
  auto word = [&](unsigned n) {
    std::uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i)
      v |= std::uint64_t(static_cast<unsigned char>(data[pos++])) << (8 * i);
    return v;
  };
  RiderState s;
  s.horse = core::EntityId(word(4));
  s.generation = word(8);
  s.epoch = std::uint32_t(word(4));
  s.phase = std::uint8_t(word(1));
  s.active_actions = std::uint8_t(word(1));
  if (!s.horse || !s.generation || s.phase > 11 || s.active_actions > 3)
    return std::nullopt;
  return s;
}
}  // namespace kcd2s::local
