// Core id types and conventions.
#ifndef KCD2S_CORE_IDS_HPP
#define KCD2S_CORE_IDS_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace kcd2s::core {

// Public/network entity identifier. Each server session hands out 1, 2, 3, ... in spawn order and
// never reuses a value, so a stale id can never name a newer entity. 0 = none. The world maps an id
// to its storage slot internally; nothing may derive a slot, player index or name from the value.
using EntityId = std::uint32_t;
using TypeId = std::uint32_t;   // crc32 of the registered type name
using Tick = std::uint32_t;     // server tick counter, 1 per sim step; compared mod 2^32
using PeerId = std::uint32_t;   // connection id (dense), distinct from EntityId
using FieldId = std::uint16_t;  // index into a type's field table

constexpr EntityId kInvalidEntity = 0;
// First id a server session assigns.
constexpr EntityId kFirstEntityId = 1;
// Last id a server session can assign. Once it has been handed out, spawning fails closed for the
// rest of the session instead of wrapping around to an id that scripts or clients may still hold.
constexpr EntityId kLastEntityId = 0xFFFFFFFFu;

// Standard IEEE 802.3 CRC-32 (reflected, poly 0xEDB88320). Used for TypeId so client and server
// derive the same id from a type name without a shared table.
std::uint32_t crc32(const void* data, std::size_t len);

// The same CRC-32, bit by bit, so a type name in the code becomes its id at compile time
// (`constexpr TypeId kPlayer = type_id("NetworkPlayer")`): hot loops compared entity types by
// computing the CRC of a literal again and again.
constexpr TypeId type_id(std::string_view name) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const char c : name) {
    crc ^= static_cast<std::uint8_t>(c);
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

}  // namespace kcd2s::core

#endif  // KCD2S_CORE_IDS_HPP
