#pragma once
// CLIENT_NATIVES (client -> server, on the control channel, once a connection's world is ready):
// what the client's game build gave it. How many of the game's functions its signatures found,
// and the names of those they did not find. After a game patch the server and its resources see
// which parts of a player's client lost their hook, instead of guessing from odd behaviour.
//
//   [tag][varuint found][varuint count][count names: varuint length, bytes]
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "kcd2s/core/bits.hpp"

namespace kcd2s::integration {

inline constexpr std::uint8_t kClientNativesTag = 175;
inline constexpr std::size_t kMaxClientNativeNames = 64;
inline constexpr std::size_t kMaxClientNativeNameBytes = 96;

struct ClientNatives {
  std::uint32_t found = 0;
  std::vector<std::string> missing;

  friend bool operator==(const ClientNatives& a, const ClientNatives& b) {
    return a.found == b.found && a.missing == b.missing;
  }
};

// Empty when a name is empty or too long, or there are too many.
inline std::vector<std::uint8_t> encode_client_natives(const ClientNatives& natives) {
  if (natives.missing.size() > kMaxClientNativeNames) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kClientNativesTag, 8);
  w.write_varuint(natives.found);
  w.write_varuint(natives.missing.size());
  for (const std::string& name : natives.missing) {
    if (name.empty() || name.size() > kMaxClientNativeNameBytes) {
      return {};
    }
    w.write_varuint(name.size());
    w.write_bytes(reinterpret_cast<const std::uint8_t*>(name.data()), name.size());
  }
  return w.bytes();
}

inline bool decode_client_natives(const std::vector<std::uint8_t>& p, ClientNatives& natives) {
  if (p.empty() || p[0] != kClientNativesTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  const std::uint64_t found = r.read_varuint();
  const std::uint64_t count = r.read_varuint();
  if (r.overrun() || found > 0xFFFFFFFFu || count > kMaxClientNativeNames) {
    return false;
  }
  ClientNatives decoded;
  decoded.found = std::uint32_t(found);
  for (std::uint64_t i = 0; i < count; ++i) {
    const std::uint64_t length = r.read_varuint();
    if (r.overrun() || length == 0 || length > kMaxClientNativeNameBytes) {
      return false;
    }
    std::string name(length, '\0');
    r.read_byte_run(reinterpret_cast<std::uint8_t*>(name.data()), length);
    if (r.overrun()) {
      return false;
    }
    for (const char c : name) {
      if (static_cast<unsigned char>(c) < 32 || c == 127) {
        return false;
      }
    }
    decoded.missing.push_back(std::move(name));
  }
  if (r.overrun() || r.bits_remaining() != 0) {
    return false;
  }
  natives = std::move(decoded);
  return true;
}

}  // namespace kcd2s::integration
