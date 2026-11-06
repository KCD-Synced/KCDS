// Shared integration constants for the headless server/client peers.
#ifndef KCD2S_INTEGRATION_PROTOCOL_HPP
#define KCD2S_INTEGRATION_PROTOCOL_HPP

#include <cstdint>
#include <vector>

#include "kcd2s/core/ids.hpp"

namespace kcd2s::integration {

// Logical channel the client uses to report the newest snapshot tick it has applied, so the server
// can advance that client's baseline (baseline ack). UnreliableSequenced =
// keep-newest, which is exactly the semantics a monotonic baseline wants. Beyond the §2 channel 0-4
// range on purpose.
constexpr std::uint8_t kChannelReplAck = 5;

inline std::vector<std::uint8_t> encode_tick(core::Tick t) {
  return {static_cast<std::uint8_t>(t & 0xFF), static_cast<std::uint8_t>((t >> 8) & 0xFF),
          static_cast<std::uint8_t>((t >> 16) & 0xFF), static_cast<std::uint8_t>((t >> 24) & 0xFF)};
}

inline core::Tick decode_tick(const std::vector<std::uint8_t>& b) {
  if (b.size() < 4)
    return 0;
  return static_cast<core::Tick>(b[0]) | (static_cast<core::Tick>(b[1]) << 8) |
         (static_cast<core::Tick>(b[2]) << 16) | (static_cast<core::Tick>(b[3]) << 24);
}

}  // namespace kcd2s::integration

#endif  // KCD2S_INTEGRATION_PROTOCOL_HPP
