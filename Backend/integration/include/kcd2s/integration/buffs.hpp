#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "kcd2s/core/bits.hpp"
#include "kcd2s/integration/guid.hpp"

// The status effects on a player's body: what the player's own game holds, reported
// to the server, which gives the resources Player.buffs, hasBuff and the buff events, and the
// server's orders on them. On the control channel (0); tags 1-20 are taken by control.hpp,
// world_config.hpp, knockout.hpp, appearance.hpp, player_identity.hpp, player_stats.hpp,
// movement_mode.hpp and progression.hpp.
//   BUFFS        : the player's own client -> server, the status effects on the body whenever
//                  they changed: each one's buff (its GUID), its instance id and whether the
//                  server added it. Only the kinds a server can claim, and what the server added.
//   BUFF_ORDER   : server -> the player's own client: Player.addBuff, removeBuff, clearBuffs and
//                  Buffs.claim, applied through the game's buff manager.
//   BUFF_BLOCKED : the player's own client -> server, an effect of a claimed kind the game tried
//                  to give the local player and the client turned down (playerBuffBlocked).
//
// Messages are byte-packed with varints (kcd2s/core/bits.hpp).

namespace kcd2s::integration {

inline constexpr std::uint8_t kBuffsTag = 21;
inline constexpr std::uint8_t kBuffOrderTag = 22;
inline constexpr std::uint8_t kBuffBlockedTag = 23;
// More status effects than a body carries at once; a report keeps the first ones.
inline constexpr std::size_t kMaxBuffs = 64;
// The game's buff classes have ids below this (rpg/buff_class.xml).
inline constexpr std::uint8_t kMaxBuffClasses = 32;
// The classes of status effect a report carries and a server can claim (rpg/buff_class.xml ids:
// injury, heal, poison, perception, overeat, alcohol, potion, food poison, unconsciousness,
// hangover, satisfaction, perfume, punishment, forced drunkenness, plague); the buff catalog's
// claimable classes (Backend/scripting/runtimes/js/buffs/catalog.json).
inline constexpr std::array<std::uint8_t, 15> kStatusBuffClasses = {5,  6,  7,  8,  9,  10, 13, 14,
                                                                    16, 17, 18, 19, 20, 21, 22};

struct BuffEntry {
  Guid buff{};
  std::uint64_t instance = 0;  // the buff manager's id of this instance
  bool server = false;         // added by a server order

  friend bool operator==(const BuffEntry& a, const BuffEntry& b) {
    return a.buff == b.buff && a.instance == b.instance && a.server == b.server;
  }
};

struct BuffOrder {
  enum class Kind : std::uint8_t {
    Add = 1,     // buff
    Remove = 2,  // buff, and the instances the last report named (none: every instance)
    Clear = 3,   // every buff of one family (tag)
    Claim = 4,   // the classes the server decides (classes; none hands them all back)
  };
  Kind kind = Kind::Add;
  Guid buff{};
  std::vector<std::uint64_t> instances;
  std::uint8_t tag = 0;
  std::vector<std::uint8_t> classes;

  friend bool operator==(const BuffOrder& a, const BuffOrder& b) {
    return a.kind == b.kind && a.buff == b.buff && a.instances == b.instances && a.tag == b.tag &&
           a.classes == b.classes;
  }
};

namespace detail {

inline void write_guid(core::BitWriter& w, const Guid& id) {
  w.write_bytes(id.data(), id.size());
}

inline Guid read_guid(core::BitReader& r) {
  Guid id{};
  for (std::uint8_t& byte : id) {
    byte = std::uint8_t(r.read_bits(8));
  }
  return id;
}

// Whether the reader took exactly the whole message.
inline bool read_whole(const core::BitReader& r) {
  return !r.overrun() && r.bits_remaining() == 0;
}

}  // namespace detail

// BUFFS: [tag][varuint count][per buff: 16 bytes GUID, varuint instance, u8 server]. Empty when
// there are more than kMaxBuffs.
inline std::vector<std::uint8_t> encode_buffs(const std::vector<BuffEntry>& buffs) {
  if (buffs.size() > kMaxBuffs) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kBuffsTag, 8);
  w.write_varuint(buffs.size());
  for (const BuffEntry& entry : buffs) {
    detail::write_guid(w, entry.buff);
    w.write_varuint(entry.instance);
    w.write_bits(entry.server ? 1 : 0, 8);
  }
  return w.bytes();
}

inline bool decode_buffs(const std::vector<std::uint8_t>& p, std::vector<BuffEntry>& buffs) {
  if (p.empty() || p[0] != kBuffsTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  const std::uint64_t count = r.read_varuint();
  if (r.overrun() || count > kMaxBuffs) {
    return false;
  }
  std::vector<BuffEntry> decoded(count);
  for (BuffEntry& entry : decoded) {
    entry.buff = detail::read_guid(r);
    entry.instance = r.read_varuint();
    const std::uint64_t server = r.read_bits(8);
    if (server > 1) {
      return false;
    }
    entry.server = server == 1;
  }
  if (!detail::read_whole(r)) {
    return false;
  }
  buffs = std::move(decoded);
  return true;
}

// BUFF_ORDER: [tag][kind][payload]; empty for an order that cannot travel.
//   Add    : 16 bytes GUID
//   Remove : 16 bytes GUID, varuint count, varuint instances
//   Clear  : u8 tag
//   Claim  : varuint count, u8 classes
inline std::vector<std::uint8_t> encode_buff_order(const BuffOrder& order) {
  using Kind = BuffOrder::Kind;
  core::BitWriter w;
  w.write_bits(kBuffOrderTag, 8);
  w.write_bits(std::uint8_t(order.kind), 8);
  switch (order.kind) {
    case Kind::Add:
      detail::write_guid(w, order.buff);
      return w.bytes();
    case Kind::Remove:
      if (order.instances.size() > kMaxBuffs) {
        return {};
      }
      detail::write_guid(w, order.buff);
      w.write_varuint(order.instances.size());
      for (const std::uint64_t instance : order.instances) {
        w.write_varuint(instance);
      }
      return w.bytes();
    case Kind::Clear:
      w.write_bits(order.tag, 8);
      return w.bytes();
    case Kind::Claim:
      if (order.classes.size() > kMaxBuffClasses) {
        return {};
      }
      w.write_varuint(order.classes.size());
      for (const std::uint8_t id : order.classes) {
        if (id >= kMaxBuffClasses) {
          return {};
        }
        w.write_bits(id, 8);
      }
      return w.bytes();
  }
  return {};
}

inline bool decode_buff_order(const std::vector<std::uint8_t>& p, BuffOrder& order) {
  using Kind = BuffOrder::Kind;
  if (p.size() < 2 || p[0] != kBuffOrderTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  BuffOrder decoded;
  decoded.kind = Kind(r.read_bits(8));
  switch (decoded.kind) {
    case Kind::Add:
      decoded.buff = detail::read_guid(r);
      break;
    case Kind::Remove: {
      decoded.buff = detail::read_guid(r);
      const std::uint64_t count = r.read_varuint();
      if (r.overrun() || count > kMaxBuffs) {
        return false;
      }
      decoded.instances.resize(count);
      for (std::uint64_t& instance : decoded.instances) {
        instance = r.read_varuint();
      }
      break;
    }
    case Kind::Clear:
      decoded.tag = std::uint8_t(r.read_bits(8));
      break;
    case Kind::Claim: {
      const std::uint64_t count = r.read_varuint();
      if (r.overrun() || count > kMaxBuffClasses) {
        return false;
      }
      decoded.classes.resize(count);
      for (std::uint8_t& id : decoded.classes) {
        id = std::uint8_t(r.read_bits(8));
        if (id >= kMaxBuffClasses) {
          return false;
        }
      }
      break;
    }
    default:
      return false;
  }
  if (!detail::read_whole(r)) {
    return false;
  }
  order = std::move(decoded);
  return true;
}

// BUFF_BLOCKED: [tag][16 bytes GUID].
inline std::vector<std::uint8_t> encode_buff_blocked(const Guid& buff) {
  std::vector<std::uint8_t> p{kBuffBlockedTag};
  p.insert(p.end(), buff.begin(), buff.end());
  return p;
}

inline bool decode_buff_blocked(const std::vector<std::uint8_t>& p, Guid& buff) {
  if (p.size() != 17 || p[0] != kBuffBlockedTag) {
    return false;
  }
  std::copy(p.begin() + 1, p.end(), buff.begin());
  return true;
}

}  // namespace kcd2s::integration
