#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "kcd2s/core/bits.hpp"
#include "kcd2s/integration/guid.hpp"

// A player's inventory: the server holds the stock, and the player's own game shows
// it. On the control channel (0); tags 1-23 are taken by control.hpp, world_config.hpp,
// knockout.hpp, appearance.hpp, player_identity.hpp, player_stats.hpp, movement_mode.hpp,
// progression.hpp and buffs.hpp.
//   INVENTORY_SHOW   : server -> the player's own client, the whole stock with its revision,
//                      whenever it changed. The client brings the game's own inventory to it.
//   INVENTORY_REPORT : the player's own client -> server. `Shown`: the revision its game shows
//                      now. `Census`: once when the player comes into the world, what the game's
//                      own inventory holds, before any stock arrived; the server takes it as the
//                      player's stock unless a resource set one already. `Changes`: what the game
//                      changed on its own since the stock it shows (units spent or dropped, an
//                      item worn down or mended, items gained). `Quickslots`: what the current
//                      outfit's quickslots hold and which are selected, when that changed.
//                      INVENTORY_SHOW carries quickslots too when a resource restored them.
//                      `Consume`: the player ate, drank or applied an item of a row in their
//                      inventory screen; the game waits for the server's verdict.
//                      `Consumed`: whether the game then used it. `Pickup`: the player took
//                      up a stack lying in the world; the game left it lying.
//                      `Drop`: the player dropped units of a row in their inventory screen;
//                      the game kept them. `PotConsume`: the player ate or drank a serving of
//                      a class from a cooking pot or a barrel; the game waits for
//                      the verdict as for `Consume`.
//                      Containers the server holds (stash.hpp):
//                      `StashOpen` the player used one (the game waits for STASH_SHOW),
//                      `StashClose` its transfer screen closed, `StashTake` units of one of its
//                      rows go to the player, `StashPut` units of one of the player's rows go
//                      into it. The game moves nothing itself.
//                      `CustomUse`: the player chose the use action on an item of a usable type
//                      a resource registered; the game did nothing itself.
//   INVENTORY_CONSUME: server -> the player's own client, the verdict on a Consume: refused,
//                      used as the game does it, or used without the game's own effects.
//   ITEM_NAMES       : server -> every client, what the game calls items (Inventory.renameItem):
//                      localization keys and the names that replace their text, the whole set
//                      for a client that came in and each change for the rest.
//
// Messages are byte-packed with varints (kcd2s/core/bits.hpp).

namespace kcd2s::integration {

inline constexpr std::uint8_t kInventoryShowTag = 24;
inline constexpr std::uint8_t kInventoryReportTag = 25;
inline constexpr std::uint8_t kItemNamesTag = 26;
inline constexpr std::uint8_t kInventoryConsumeTag = 27;
// Item names a server can set at once, the longest key, and the longest text (a registered item
// type's description).
inline constexpr std::size_t kMaxItemNames = 12288;
inline constexpr std::size_t kMaxItemNameBytes = 128;
inline constexpr std::size_t kMaxItemTextBytes = 4096;
// The server's limit on rows in one inventory.
inline constexpr std::size_t kMaxInventoryRows = 2048;
// A census travels from the client, whose messages are at most 64 KiB: a body carries far fewer
// rows than this.
inline constexpr std::size_t kMaxCensusRows = 1024;
inline constexpr std::size_t kMaxRowIdBytes = 64;
// Changes in one report; a client reports four times a second at most.
inline constexpr std::size_t kMaxNativeChanges = 256;
// Weapon and item quickslots of an outfit.
inline constexpr std::size_t kQuickslots = 4;

struct InventoryRowState {
  std::string id;  // the row's id; empty in a census
  Guid item{};
  std::uint32_t amount = 0;
  std::uint8_t quality = 1;
  float health = 1.0f;
  std::uint32_t equipped = 0;

  friend bool operator==(const InventoryRowState& a, const InventoryRowState& b) {
    return a.id == b.id && a.item == b.item && a.amount == b.amount && a.quality == b.quality &&
           a.health == b.health && a.equipped == b.equipped;
  }
};

// One change the game made to its own inventory.
struct InventoryNativeChange {
  enum class Kind : std::uint8_t {
    Lost = 1,      // `amount` units of row `id` went (eaten, drunk, applied, fired, dropped)
    Worn = 2,      // `amount` units of row `id` now have `quality` and `health`
    Gained = 3,    // `amount` new units of class `item` with `quality` and `health`
    Equipped = 4,  // the body wears or holds `amount` units of row `id` (0: none)
  };
  Kind kind = Kind::Lost;
  std::string id;  // the row (Lost, Worn)
  Guid item{};     // the class (Gained)
  std::uint32_t amount = 0;
  std::uint8_t quality = 1;
  float health = 1.0f;

  friend bool operator==(const InventoryNativeChange& a, const InventoryNativeChange& b) {
    return a.kind == b.kind && a.id == b.id && a.item == b.item && a.amount == b.amount &&
           a.quality == b.quality && a.health == b.health;
  }
};

// The current outfit's quickslots: rows by id ("" for an empty slot), four weapon
// slots with a primary and a secondary each and four item slots, and the selected slot of each.
struct InventoryQuickslots {
  std::array<std::string, kQuickslots> primary;
  std::array<std::string, kQuickslots> secondary;
  std::array<std::string, kQuickslots> items;
  std::uint8_t active_weapon = 0;
  std::uint8_t active_item = 0;

  friend bool operator==(const InventoryQuickslots& a, const InventoryQuickslots& b) {
    return a.primary == b.primary && a.secondary == b.secondary && a.items == b.items &&
           a.active_weapon == b.active_weapon && a.active_item == b.active_item;
  }
  friend bool operator!=(const InventoryQuickslots& a, const InventoryQuickslots& b) {
    return !(a == b);
  }
};

struct InventoryShow {
  std::uint64_t revision = 0;
  // A resource set this stock (a restore): on the first stock of a connection, what the game holds
  // and the stock does not goes. Otherwise the stock grew from the census and what the game holds
  // beyond it is reported as gained.
  bool replace = false;
  std::vector<InventoryRowState> rows;
  // Quickslots a resource restored; none leaves them to the game.
  std::optional<InventoryQuickslots> quickslots;

  friend bool operator==(const InventoryShow& a, const InventoryShow& b) {
    return a.revision == b.revision && a.replace == b.replace && a.rows == b.rows &&
           a.quickslots == b.quickslots;
  }
};

struct InventoryReport {
  enum class Kind : std::uint8_t {
    Shown = 1,        // revision
    Census = 2,       // rows (without ids)
    Changes = 3,      // revision (the stock the changes started from), changes
    Quickslots = 4,   // quickslots
    Consume = 5,      // request, row, revision (the stock the game shows)
    Consumed = 6,     // request, used
    Pickup = 7,       // request: the stack's network id
    Drop = 8,         // row, request: the units
    PotConsume = 9,   // request, item: the serving's class, pot: the fireplace (zero: unknown)
    StashOpen = 10,   // request: the container's network id
    StashClose = 11,  // request: the container's network id
    StashTake = 12,   // request: the container's network id, row (the container's), units
    StashPut = 13,    // request: the container's network id, row (the player's), units
    CustomUse = 14,   // row, revision (the stock the game shows)
    // The owner put a piece of tack on their horse in the game's horse inventory screen (issue).
    // request: the horse's network id, row: the row of the piece, used: the row is the
    // horse's saddlebags' (else the player's).
    HorseGearEquip = 15,
    // The owner took a piece of tack off. request: the horse's network id, item: its class.
    HorseGearRemove = 16,
  };
  Kind kind = Kind::Shown;
  std::uint64_t revision = 0;
  std::vector<InventoryRowState> rows;
  std::vector<InventoryNativeChange> changes;
  InventoryQuickslots quickslots;
  std::uint32_t request = 0;
  std::string row;
  bool used = false;
  Guid item{};
  Guid pot{};
  std::uint32_t units = 0;

  friend bool operator==(const InventoryReport& a, const InventoryReport& b) {
    return a.kind == b.kind && a.revision == b.revision && a.rows == b.rows &&
           a.changes == b.changes && a.quickslots == b.quickslots && a.request == b.request &&
           a.row == b.row && a.used == b.used && a.item == b.item && a.pot == b.pot &&
           a.units == b.units;
  }
};

// The server's verdict on a Consume.
struct InventoryConsume {
  enum class Verdict : std::uint8_t {
    Refused = 0,  // the game does not use the item
    Native = 1,   // the game uses it as it does by itself
    Plain = 2,    // the game uses it up without its own effects
  };
  std::uint32_t request = 0;
  Verdict verdict = Verdict::Refused;

  friend bool operator==(const InventoryConsume& a, const InventoryConsume& b) {
    return a.request == b.request && a.verdict == b.verdict;
  }
};

// What the game calls items: `full` replaces every name set before, otherwise these change; an
// empty name gives the key its own text back.
struct ItemNames {
  bool full = false;
  std::vector<std::pair<std::string, std::string>> names;  // localization key, name

  friend bool operator==(const ItemNames& a, const ItemNames& b) {
    return a.full == b.full && a.names == b.names;
  }
};

namespace inventory_detail {

inline void write_row(core::BitWriter& w, const InventoryRowState& row, bool with_id) {
  if (with_id) {
    w.write_varuint(row.id.size());
    w.write_bytes(reinterpret_cast<const std::uint8_t*>(row.id.data()), row.id.size());
  }
  w.write_bytes(row.item.data(), row.item.size());
  w.write_varuint(row.amount);
  w.write_bits(row.quality, 8);
  std::uint32_t health_bits = 0;
  std::memcpy(&health_bits, &row.health, sizeof(health_bits));
  w.write_bits(health_bits, 32);
  w.write_varuint(row.equipped);
}

inline bool read_row(core::BitReader& r, InventoryRowState& row, bool with_id) {
  if (with_id) {
    const std::uint64_t length = r.read_varuint();
    if (r.overrun() || length == 0 || length > kMaxRowIdBytes) {
      return false;
    }
    row.id.resize(length);
    for (char& c : row.id) {
      c = char(r.read_bits(8));
    }
  }
  for (std::uint8_t& byte : row.item) {
    byte = std::uint8_t(r.read_bits(8));
  }
  const std::uint64_t amount = r.read_varuint();
  row.quality = std::uint8_t(r.read_bits(8));
  const auto health_bits = std::uint32_t(r.read_bits(32));
  std::memcpy(&row.health, &health_bits, sizeof(row.health));
  const std::uint64_t equipped = r.read_varuint();
  if (r.overrun() || amount == 0 || amount > 0xFFFFFFFFu || equipped > amount || row.quality == 0 ||
      !(row.health >= 0.0f && row.health <= 1.0f)) {
    return false;
  }
  row.amount = std::uint32_t(amount);
  row.equipped = std::uint32_t(equipped);
  return true;
}

inline void write_change(core::BitWriter& w, const InventoryNativeChange& change) {
  w.write_bits(std::uint8_t(change.kind), 8);
  if (change.kind == InventoryNativeChange::Kind::Gained) {
    w.write_bytes(change.item.data(), change.item.size());
  } else {
    w.write_varuint(change.id.size());
    w.write_bytes(reinterpret_cast<const std::uint8_t*>(change.id.data()), change.id.size());
  }
  w.write_varuint(change.amount);
  w.write_bits(change.quality, 8);
  std::uint32_t health_bits = 0;
  std::memcpy(&health_bits, &change.health, sizeof(health_bits));
  w.write_bits(health_bits, 32);
}

inline bool read_change(core::BitReader& r, InventoryNativeChange& change) {
  using Kind = InventoryNativeChange::Kind;
  change.kind = Kind(r.read_bits(8));
  if (change.kind == Kind::Gained) {
    for (std::uint8_t& byte : change.item) {
      byte = std::uint8_t(r.read_bits(8));
    }
  } else if (change.kind == Kind::Lost || change.kind == Kind::Worn ||
             change.kind == Kind::Equipped) {
    const std::uint64_t length = r.read_varuint();
    if (r.overrun() || length == 0 || length > kMaxRowIdBytes) {
      return false;
    }
    change.id.resize(length);
    for (char& c : change.id) {
      c = char(r.read_bits(8));
    }
  } else {
    return false;
  }
  const std::uint64_t amount = r.read_varuint();
  change.quality = std::uint8_t(r.read_bits(8));
  const auto health_bits = std::uint32_t(r.read_bits(32));
  std::memcpy(&change.health, &health_bits, sizeof(change.health));
  const bool counts_none = change.kind == Kind::Equipped;
  if (r.overrun() || (amount == 0 && !counts_none) || amount > 0xFFFFFFFFu || change.quality == 0 ||
      !(change.health >= 0.0f && change.health <= 1.0f)) {
    return false;
  }
  change.amount = std::uint32_t(amount);
  return true;
}

inline bool read_whole(const core::BitReader& r) {
  return !r.overrun() && r.bits_remaining() == 0;
}

inline bool write_slot_id(core::BitWriter& w, const std::string& id) {
  if (id.size() > kMaxRowIdBytes) {
    return false;
  }
  w.write_varuint(id.size());
  w.write_bytes(reinterpret_cast<const std::uint8_t*>(id.data()), id.size());
  return true;
}

// Quickslots: twelve row ids (varuint length, bytes; 0 for an empty slot): each weapon slot's
// primary and secondary in turn, then the item slots; then u8 selected weapon slot and u8
// selected item slot. False for quickslots that cannot travel.
inline bool write_quickslots(core::BitWriter& w, const InventoryQuickslots& slots) {
  for (std::size_t slot = 0; slot < kQuickslots; ++slot) {
    if (!write_slot_id(w, slots.primary[slot]) || !write_slot_id(w, slots.secondary[slot])) {
      return false;
    }
  }
  for (const std::string& id : slots.items) {
    if (!write_slot_id(w, id)) {
      return false;
    }
  }
  if (slots.active_weapon >= kQuickslots || slots.active_item >= kQuickslots) {
    return false;
  }
  w.write_bits(slots.active_weapon, 8);
  w.write_bits(slots.active_item, 8);
  return true;
}

inline bool read_slot_id(core::BitReader& r, std::string& id) {
  const std::uint64_t length = r.read_varuint();
  if (r.overrun() || length > kMaxRowIdBytes) {
    return false;
  }
  id.resize(length);
  for (char& c : id) {
    c = char(r.read_bits(8));
  }
  return !r.overrun();
}

inline bool read_quickslots(core::BitReader& r, InventoryQuickslots& slots) {
  for (std::size_t slot = 0; slot < kQuickslots; ++slot) {
    if (!read_slot_id(r, slots.primary[slot]) || !read_slot_id(r, slots.secondary[slot])) {
      return false;
    }
  }
  for (std::string& id : slots.items) {
    if (!read_slot_id(r, id)) {
      return false;
    }
  }
  slots.active_weapon = std::uint8_t(r.read_bits(8));
  slots.active_item = std::uint8_t(r.read_bits(8));
  return !r.overrun() && slots.active_weapon < kQuickslots && slots.active_item < kQuickslots;
}

}  // namespace inventory_detail

// INVENTORY_SHOW: [tag][varuint revision][u8 replace][varuint count][rows: varuint id length,
// id bytes, 16 bytes GUID, varuint amount, u8 quality, u32 health (float bits), varuint
// equipped][u8 quickslots present][quickslots]. Empty when there are more than
// kMaxInventoryRows.
inline std::vector<std::uint8_t> encode_inventory_show(const InventoryShow& show) {
  if (show.rows.size() > kMaxInventoryRows) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kInventoryShowTag, 8);
  w.write_varuint(show.revision);
  w.write_bits(show.replace ? 1 : 0, 8);
  w.write_varuint(show.rows.size());
  for (const InventoryRowState& row : show.rows) {
    if (row.id.empty() || row.id.size() > kMaxRowIdBytes) {
      return {};
    }
    inventory_detail::write_row(w, row, true);
  }
  w.write_bits(show.quickslots ? 1 : 0, 8);
  if (show.quickslots && !inventory_detail::write_quickslots(w, *show.quickslots)) {
    return {};
  }
  return w.bytes();
}

inline bool decode_inventory_show(const std::vector<std::uint8_t>& p, InventoryShow& show) {
  if (p.empty() || p[0] != kInventoryShowTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  InventoryShow decoded;
  decoded.revision = r.read_varuint();
  const std::uint64_t replace = r.read_bits(8);
  if (replace > 1) {
    return false;
  }
  decoded.replace = replace == 1;
  const std::uint64_t count = r.read_varuint();
  if (r.overrun() || count > kMaxInventoryRows) {
    return false;
  }
  decoded.rows.resize(count);
  for (InventoryRowState& row : decoded.rows) {
    if (!inventory_detail::read_row(r, row, true)) {
      return false;
    }
  }
  const std::uint64_t has_quickslots = r.read_bits(8);
  if (has_quickslots > 1) {
    return false;
  }
  if (has_quickslots == 1) {
    decoded.quickslots.emplace();
    if (!inventory_detail::read_quickslots(r, *decoded.quickslots)) {
      return false;
    }
  }
  if (!inventory_detail::read_whole(r)) {
    return false;
  }
  show = std::move(decoded);
  return true;
}

// INVENTORY_REPORT: [tag][kind][payload]; empty for a report that cannot travel.
//   Shown  : varuint revision
//   Census : varuint count, rows as in INVENTORY_SHOW without the id
//   Changes: varuint revision, varuint count, changes: u8 kind, the row id (varuint length,
//            bytes) or for Gained the 16 bytes class GUID, varuint amount, u8 quality, u32 health
//   Quickslots: quickslots as in INVENTORY_SHOW
//   Consume : varuint request, varuint row id length, row id bytes, varuint revision
//   Consumed: varuint request, u8 used
//   Pickup  : varuint the stack's network id
//   Drop    : varuint row id length, row id bytes, varuint units
//   PotConsume: varuint request, 16 bytes class GUID, u8 1 and the fireplace's 16 bytes GUID, or 0
//   StashOpen, StashClose: varuint the container's network id
//   StashTake, StashPut  : varuint the container's network id, varuint row id length, row id
//                          bytes, varuint units
//   CustomUse: varuint row id length, row id bytes, varuint revision
//   HorseGearEquip : varuint the horse's network id, u8 1 for a saddlebags row (0 the player's),
//                    varuint row id length, row id bytes
//   HorseGearRemove: varuint the horse's network id, 16 bytes class GUID
inline std::vector<std::uint8_t> encode_inventory_report(const InventoryReport& report) {
  using Kind = InventoryReport::Kind;
  core::BitWriter w;
  w.write_bits(kInventoryReportTag, 8);
  w.write_bits(std::uint8_t(report.kind), 8);
  switch (report.kind) {
    case Kind::Shown:
      w.write_varuint(report.revision);
      return w.bytes();
    case Kind::Census:
      if (report.rows.size() > kMaxCensusRows) {
        return {};
      }
      w.write_varuint(report.rows.size());
      for (const InventoryRowState& row : report.rows) {
        inventory_detail::write_row(w, row, false);
      }
      return w.bytes();
    case Kind::Changes:
      if (report.changes.empty() || report.changes.size() > kMaxNativeChanges) {
        return {};
      }
      w.write_varuint(report.revision);
      w.write_varuint(report.changes.size());
      for (const InventoryNativeChange& change : report.changes) {
        if (change.kind != InventoryNativeChange::Kind::Gained &&
            (change.id.empty() || change.id.size() > kMaxRowIdBytes)) {
          return {};
        }
        inventory_detail::write_change(w, change);
      }
      return w.bytes();
    case Kind::Quickslots:
      if (!inventory_detail::write_quickslots(w, report.quickslots)) {
        return {};
      }
      return w.bytes();
    case Kind::Consume:
      if (report.row.empty() || !inventory_detail::write_slot_id(w, report.row)) {
        return {};
      }
      w.write_varuint(report.request);
      w.write_varuint(report.revision);
      return w.bytes();
    case Kind::Consumed:
      w.write_varuint(report.request);
      w.write_bits(report.used ? 1 : 0, 8);
      return w.bytes();
    case Kind::Pickup:
      w.write_varuint(report.request);
      return w.bytes();
    case Kind::PotConsume: {
      w.write_varuint(report.request);
      w.write_bytes(report.item.data(), report.item.size());
      const bool known = report.pot != Guid{};
      w.write_bits(known ? 1 : 0, 8);
      if (known) {
        w.write_bytes(report.pot.data(), report.pot.size());
      }
      return w.bytes();
    }
    case Kind::Drop:
      if (report.row.empty() || report.request == 0 ||
          !inventory_detail::write_slot_id(w, report.row)) {
        return {};
      }
      w.write_varuint(report.request);
      return w.bytes();
    case Kind::CustomUse:
      if (report.row.empty() || !inventory_detail::write_slot_id(w, report.row)) {
        return {};
      }
      w.write_varuint(report.revision);
      return w.bytes();
    case Kind::StashOpen:
    case Kind::StashClose:
      if (report.request == 0) {
        return {};
      }
      w.write_varuint(report.request);
      return w.bytes();
    case Kind::StashTake:
    case Kind::StashPut:
      if (report.request == 0 || report.row.empty() || report.units == 0) {
        return {};
      }
      w.write_varuint(report.request);
      if (!inventory_detail::write_slot_id(w, report.row)) {
        return {};
      }
      w.write_varuint(report.units);
      return w.bytes();
    case Kind::HorseGearEquip:
      if (report.request == 0 || report.row.empty()) {
        return {};
      }
      w.write_varuint(report.request);
      w.write_bits(report.used ? 1 : 0, 8);
      if (!inventory_detail::write_slot_id(w, report.row)) {
        return {};
      }
      return w.bytes();
    case Kind::HorseGearRemove:
      if (report.request == 0 || report.item == Guid{}) {
        return {};
      }
      w.write_varuint(report.request);
      w.write_bytes(report.item.data(), report.item.size());
      return w.bytes();
  }
  return {};
}

inline bool decode_inventory_report(const std::vector<std::uint8_t>& p, InventoryReport& report) {
  using Kind = InventoryReport::Kind;
  if (p.size() < 2 || p[0] != kInventoryReportTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  InventoryReport decoded;
  decoded.kind = Kind(r.read_bits(8));
  switch (decoded.kind) {
    case Kind::Shown:
      decoded.revision = r.read_varuint();
      break;
    case Kind::Census: {
      const std::uint64_t count = r.read_varuint();
      if (r.overrun() || count > kMaxCensusRows) {
        return false;
      }
      decoded.rows.resize(count);
      for (InventoryRowState& row : decoded.rows) {
        if (!inventory_detail::read_row(r, row, false)) {
          return false;
        }
      }
      break;
    }
    case Kind::Changes: {
      decoded.revision = r.read_varuint();
      const std::uint64_t count = r.read_varuint();
      if (r.overrun() || count == 0 || count > kMaxNativeChanges) {
        return false;
      }
      decoded.changes.resize(count);
      for (InventoryNativeChange& change : decoded.changes) {
        if (!inventory_detail::read_change(r, change)) {
          return false;
        }
      }
      break;
    }
    case Kind::Quickslots:
      if (!inventory_detail::read_quickslots(r, decoded.quickslots)) {
        return false;
      }
      break;
    case Kind::Consume: {
      if (!inventory_detail::read_slot_id(r, decoded.row) || decoded.row.empty()) {
        return false;
      }
      const std::uint64_t request = r.read_varuint();
      decoded.revision = r.read_varuint();
      if (r.overrun() || request > 0xFFFFFFFFu) {
        return false;
      }
      decoded.request = std::uint32_t(request);
      break;
    }
    case Kind::PotConsume: {
      const std::uint64_t request = r.read_varuint();
      for (std::uint8_t& byte : decoded.item) {
        byte = std::uint8_t(r.read_bits(8));
      }
      const auto known = r.read_bits(8);
      if (known == 1) {
        for (std::uint8_t& byte : decoded.pot) {
          byte = std::uint8_t(r.read_bits(8));
        }
      }
      if (r.overrun() || request > 0xFFFFFFFFu || known > 1 ||
          (known == 1 && decoded.pot == Guid{})) {
        return false;
      }
      decoded.request = std::uint32_t(request);
      break;
    }
    case Kind::Drop: {
      if (!inventory_detail::read_slot_id(r, decoded.row) || decoded.row.empty()) {
        return false;
      }
      const std::uint64_t units = r.read_varuint();
      if (r.overrun() || units == 0 || units > 0xFFFFFFFFu) {
        return false;
      }
      decoded.request = std::uint32_t(units);
      break;
    }
    case Kind::Pickup: {
      const std::uint64_t stack = r.read_varuint();
      if (r.overrun() || stack == 0 || stack > 0xFFFFFFFFu) {
        return false;
      }
      decoded.request = std::uint32_t(stack);
      break;
    }
    case Kind::CustomUse:
      if (!inventory_detail::read_slot_id(r, decoded.row) || decoded.row.empty()) {
        return false;
      }
      decoded.revision = r.read_varuint();
      break;
    case Kind::StashOpen:
    case Kind::StashClose: {
      const std::uint64_t stash = r.read_varuint();
      if (r.overrun() || stash == 0 || stash > 0xFFFFFFFFu) {
        return false;
      }
      decoded.request = std::uint32_t(stash);
      break;
    }
    case Kind::StashTake:
    case Kind::StashPut: {
      const std::uint64_t stash = r.read_varuint();
      if (r.overrun() || stash == 0 || stash > 0xFFFFFFFFu ||
          !inventory_detail::read_slot_id(r, decoded.row) || decoded.row.empty()) {
        return false;
      }
      const std::uint64_t units = r.read_varuint();
      if (r.overrun() || units == 0 || units > 0xFFFFFFFFu) {
        return false;
      }
      decoded.request = std::uint32_t(stash);
      decoded.units = std::uint32_t(units);
      break;
    }
    case Kind::HorseGearEquip: {
      const std::uint64_t horse = r.read_varuint();
      const std::uint64_t bags = r.read_bits(8);
      if (r.overrun() || horse == 0 || horse > 0xFFFFFFFFu || bags > 1 ||
          !inventory_detail::read_slot_id(r, decoded.row) || decoded.row.empty()) {
        return false;
      }
      decoded.request = std::uint32_t(horse);
      decoded.used = bags == 1;
      break;
    }
    case Kind::HorseGearRemove: {
      const std::uint64_t horse = r.read_varuint();
      if (r.overrun() || horse == 0 || horse > 0xFFFFFFFFu) {
        return false;
      }
      for (std::uint8_t& byte : decoded.item) {
        byte = std::uint8_t(r.read_bits(8));
      }
      if (r.overrun() || decoded.item == Guid{}) {
        return false;
      }
      decoded.request = std::uint32_t(horse);
      break;
    }
    case Kind::Consumed: {
      const std::uint64_t request = r.read_varuint();
      const std::uint64_t used = r.read_bits(8);
      if (r.overrun() || request > 0xFFFFFFFFu || used > 1) {
        return false;
      }
      decoded.request = std::uint32_t(request);
      decoded.used = used == 1;
      break;
    }
    default:
      return false;
  }
  if (!inventory_detail::read_whole(r)) {
    return false;
  }
  report = std::move(decoded);
  return true;
}

namespace inventory_detail {

inline void write_text(core::BitWriter& w, const std::string& text) {
  w.write_varuint(text.size());
  w.write_bytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

inline bool read_text(core::BitReader& r, std::string& text, bool allow_empty,
                      std::size_t limit = kMaxItemNameBytes) {
  const std::uint64_t length = r.read_varuint();
  if (r.overrun() || length > limit || (length == 0 && !allow_empty)) {
    return false;
  }
  text.resize(length);
  for (char& c : text) {
    c = char(r.read_bits(8));
  }
  return !r.overrun();
}

}  // namespace inventory_detail

// ITEM_NAMES: [tag][u8 full][varuint count][names: varuint key length, key bytes, varuint name
// length, name bytes (0: the key's own text)]. Empty for names that cannot travel.
inline std::vector<std::uint8_t> encode_item_names(const ItemNames& names) {
  if (names.names.size() > kMaxItemNames) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kItemNamesTag, 8);
  w.write_bits(names.full ? 1 : 0, 8);
  w.write_varuint(names.names.size());
  for (const auto& [key, name] : names.names) {
    if (key.empty() || key.size() > kMaxItemNameBytes || name.size() > kMaxItemTextBytes) {
      return {};
    }
    inventory_detail::write_text(w, key);
    inventory_detail::write_text(w, name);
  }
  return w.bytes();
}

inline bool decode_item_names(const std::vector<std::uint8_t>& p, ItemNames& names) {
  if (p.size() < 3 || p[0] != kItemNamesTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  ItemNames decoded;
  const std::uint64_t full = r.read_bits(8);
  const std::uint64_t count = r.read_varuint();
  if (r.overrun() || full > 1 || count > kMaxItemNames) {
    return false;
  }
  decoded.full = full == 1;
  decoded.names.resize(count);
  for (auto& [key, name] : decoded.names) {
    if (!inventory_detail::read_text(r, key, false) ||
        !inventory_detail::read_text(r, name, true, kMaxItemTextBytes)) {
      return false;
    }
  }
  if (!inventory_detail::read_whole(r)) {
    return false;
  }
  names = std::move(decoded);
  return true;
}

// INVENTORY_CONSUME: [tag][varuint request][u8 verdict].
inline std::vector<std::uint8_t> encode_inventory_consume(const InventoryConsume& consume) {
  core::BitWriter w;
  w.write_bits(kInventoryConsumeTag, 8);
  w.write_varuint(consume.request);
  w.write_bits(std::uint8_t(consume.verdict), 8);
  return w.bytes();
}

inline bool decode_inventory_consume(const std::vector<std::uint8_t>& p,
                                     InventoryConsume& consume) {
  if (p.empty() || p[0] != kInventoryConsumeTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  const std::uint64_t request = r.read_varuint();
  const std::uint64_t verdict = r.read_bits(8);
  if (request > 0xFFFFFFFFu || verdict > 2 || !inventory_detail::read_whole(r)) {
    return false;
  }
  consume.request = std::uint32_t(request);
  consume.verdict = InventoryConsume::Verdict(verdict);
  return true;
}

}  // namespace kcd2s::integration
