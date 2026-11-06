#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "kcd2s/core/bits.hpp"
#include "kcd2s/integration/guid.hpp"
#include "kcd2s/integration/inventory.hpp"

// Vendors the server holds (Vendor): a price list, a purse and
// what the vendor buys, traded on the game's own shop screen of one player. The screen is a
// preview: the server re-prices the basket the player confirms, settles it and moves everything
// itself. Every message is on the control channel (0).
//
//   VENDOR_SHOW   : server -> the player's client: what `session` shows (the shelf, its prices,
//                   what the vendor pays for the player's items, its purse). With `open` the client
//                   brings the game's shop screen up; without it the shelf of a screen that is up
//                   is refreshed. Every shelf carries the vendor's revision, which a basket echoes.
//   VENDOR_CLOSE  : server -> the player's client: take `session`'s screen down.
//   VENDOR_TRADE  : client -> server: the basket the player confirmed with the screen's Trade:
//   units
//                   of shelf rows bought and units of the player's own inventory rows sold. The
//                   game moved nothing itself.
//   VENDOR_CLOSED : client -> server: the screen of `session` went away on the player's side: 0 the
//                   player closed it, 3 the game could not bring it up, 4 the world changed under
//                   it.
//   VENDOR_HAGGLE : client -> server: a price the player offered in the game's haggle for the
//                   basket on the screen of a vendor that haggles. The server
//                   answers each one.
//   VENDOR_HAGGLE_REPLY : server -> the player's client: the trader's answer to an offer: the
//                   price agreed, a counter price, a mock of an offer made twice, or a refusal,
//                   with the trader's impatience. A basket confirmed after an agreement is settled
//                   at the agreed price.
//
// Client/runtime/vendors/ implements the client side.
namespace kcd2s::integration {

inline constexpr std::uint8_t kVendorShowTag = 125;
inline constexpr std::uint8_t kVendorCloseTag = 126;
inline constexpr std::uint8_t kVendorTradeTag = 127;
inline constexpr std::uint8_t kVendorClosedTag = 128;
inline constexpr std::uint8_t kVendorHaggleTag = 123;
inline constexpr std::uint8_t kVendorHaggleReplyTag = 124;

// Rows a shelf holds, classes a vendor buys, and lines of one side of a basket at most.
inline constexpr std::size_t kMaxVendorRows = 128;
inline constexpr std::size_t kMaxVendorBuyRows = 128;
inline constexpr std::size_t kMaxVendorTradeLines = 256;
// The highest price of one unit, and the most a purse holds, in money units.
inline constexpr std::uint32_t kMaxVendorPrice = 1000000000;
// VENDOR_TRADEs, VENDOR_CLOSEDs and VENDOR_HAGGLEs one client may send in a second; more are
// dropped.
inline constexpr unsigned kMaxVendorReportsPerSecond = 8;
// The rounds of one haggle at most, and the trader's impatience at its highest.
inline constexpr std::uint32_t kMaxVendorHaggleRounds = 64;
inline constexpr std::uint8_t kMaxVendorHaggleMood = 100;

// Why a screen went away on the player's side, as vendorClosed reports it.
enum class VendorClosedReason : std::uint8_t {
  Player = 0,       // the player closed the screen (Trade closes it too)
  Failed = 3,       // the game could not bring the screen up
  WorldChanged = 4  // the world changed under it, or the connection ended
};

// One row of the shelf and what one unit of it costs.
struct VendorShelfRow {
  InventoryRowState row;
  std::uint32_t price = 0;

  friend bool operator==(const VendorShelfRow& a, const VendorShelfRow& b) {
    return a.row == b.row && a.price == b.price;
  }
};

// One class the vendor buys and what it pays for one unit.
struct VendorBuyPrice {
  Guid item{};
  std::uint32_t price = 0;

  friend bool operator==(const VendorBuyPrice& a, const VendorBuyPrice& b) {
    return a.item == b.item && a.price == b.price;
  }
};

// VENDOR_SHOW.
struct VendorShow {
  std::uint32_t session = 0;
  bool open = false;           // bring the screen up now
  bool buys = true;            // the vendor takes the player's items
  bool purse_limited = true;   // false: a purse that never runs out
  std::uint32_t npc = 0;       // the selling NPC's network id, 0 without an NPC
  bool keeper_player = false;  // `npc` is a player's network id (a player's trade)
  bool haggles = false;        // the vendor haggles: the screen's haggle asks the server
  std::uint64_t revision = 0;
  std::uint32_t purse = 0;  // money units the vendor holds, when limited
  Guid money{};             // the class of the game's money item
  std::vector<VendorShelfRow> shelf;
  std::vector<VendorBuyPrice> buy_prices;

  friend bool operator==(const VendorShow& a, const VendorShow& b) {
    return a.session == b.session && a.open == b.open && a.buys == b.buys &&
           a.purse_limited == b.purse_limited && a.npc == b.npc &&
           a.keeper_player == b.keeper_player && a.haggles == b.haggles &&
           a.revision == b.revision && a.purse == b.purse && a.money == b.money &&
           a.shelf == b.shelf && a.buy_prices == b.buy_prices;
  }
};

// VENDOR_CLOSE.
struct VendorClose {
  std::uint32_t session = 0;

  friend bool operator==(const VendorClose& a, const VendorClose& b) {
    return a.session == b.session;
  }
};

// The trader's answer to a haggle offer, as the game's haggle names its reactions.
enum class VendorHaggleAnswer : std::uint8_t {
  Accept = 1,   // the offer is taken: `price` is the agreed price
  Counter = 2,  // the trader asks `price` instead
  Mock = 3,     // the same offer again: the trader still asks `price`
  Refuse = 4    // the trader haggles no more in this session
};

// VENDOR_HAGGLE_REPLY. Prices are what the player pays, in money units; negative: the vendor pays.
struct VendorHaggleReply {
  std::uint32_t session = 0;
  std::uint32_t round = 0;  // the offer's round it answers
  VendorHaggleAnswer answer = VendorHaggleAnswer::Refuse;
  std::int64_t price = 0;
  std::uint8_t mood = 0;  // the trader's impatience, 0 calm to kMaxVendorHaggleMood leaving

  friend bool operator==(const VendorHaggleReply& a, const VendorHaggleReply& b) {
    return a.session == b.session && a.round == b.round && a.answer == b.answer &&
           a.price == b.price && a.mood == b.mood;
  }
};

// What the server told the client, in the order it was told.
using VendorOrder = std::variant<VendorShow, VendorClose, VendorHaggleReply>;

// Units of one row in a basket: a shelf row bought, or a row of the player's inventory sold.
struct VendorTradeLine {
  std::string row;
  std::uint32_t units = 0;

  friend bool operator==(const VendorTradeLine& a, const VendorTradeLine& b) {
    return a.row == b.row && a.units == b.units;
  }
};

// VENDOR_HAGGLE: the price the player offers for the basket, as VENDOR_TRADE's lines.
struct VendorHaggle {
  std::uint32_t session = 0;
  std::uint64_t revision = 0;  // the shelf the screen showed
  std::uint32_t round = 0;     // 1 for the first offer of this haggle
  std::int64_t offered = 0;    // what the player pays; negative: the vendor pays
  std::vector<VendorTradeLine> bought;
  std::vector<VendorTradeLine> sold;

  friend bool operator==(const VendorHaggle& a, const VendorHaggle& b) {
    return a.session == b.session && a.revision == b.revision && a.round == b.round &&
           a.offered == b.offered && a.bought == b.bought && a.sold == b.sold;
  }
};

// VENDOR_TRADE.
struct VendorTrade {
  std::uint32_t session = 0;
  std::uint64_t revision = 0;  // the shelf the screen showed
  std::vector<VendorTradeLine> bought;
  std::vector<VendorTradeLine> sold;

  friend bool operator==(const VendorTrade& a, const VendorTrade& b) {
    return a.session == b.session && a.revision == b.revision && a.bought == b.bought &&
           a.sold == b.sold;
  }
};

// VENDOR_CLOSED.
struct VendorClosed {
  std::uint32_t session = 0;
  VendorClosedReason reason = VendorClosedReason::Player;

  friend bool operator==(const VendorClosed& a, const VendorClosed& b) {
    return a.session == b.session && a.reason == b.reason;
  }
};

namespace vendor_detail {

// A session id from a varuint: 1 .. 2^32 - 1.
inline bool read_session(core::BitReader& r, std::uint32_t& session) {
  const std::uint64_t value = r.read_varuint();
  if (r.overrun() || value == 0 || value > 0xFFFFFFFFu) {
    return false;
  }
  session = std::uint32_t(value);
  return true;
}

// A price or a purse: 0 .. kMaxVendorPrice.
inline bool read_price(core::BitReader& r, std::uint32_t& price) {
  const std::uint64_t value = r.read_varuint();
  if (r.overrun() || value > kMaxVendorPrice) {
    return false;
  }
  price = std::uint32_t(value);
  return true;
}

inline void write_guid(core::BitWriter& w, const Guid& id) {
  w.write_bytes(id.data(), id.size());
}

inline void read_guid(core::BitReader& r, Guid& id) {
  for (std::uint8_t& byte : id) {
    byte = std::uint8_t(r.read_bits(8));
  }
}

inline bool read_whole(const core::BitReader& r) {
  return !r.overrun() && r.bits_remaining() == 0;
}

// A haggle price: [u8 sign: 0 the player pays, 1 the vendor pays][varuint amount], the amount at
// most kMaxVendorPrice.
inline bool write_signed_price(core::BitWriter& w, std::int64_t price) {
  const std::uint64_t amount = price < 0 ? std::uint64_t(-price) : std::uint64_t(price);
  if (amount > kMaxVendorPrice) {
    return false;
  }
  w.write_bits(price < 0 ? 1 : 0, 8);
  w.write_varuint(amount);
  return true;
}

inline bool read_signed_price(core::BitReader& r, std::int64_t& price) {
  const std::uint64_t sign = r.read_bits(8);
  std::uint32_t amount = 0;
  if (r.overrun() || sign > 1 || !read_price(r, amount)) {
    return false;
  }
  price = sign == 1 ? -std::int64_t(amount) : std::int64_t(amount);
  return true;
}

// A haggle round: 1 .. kMaxVendorHaggleRounds.
inline bool read_round(core::BitReader& r, std::uint32_t& round) {
  const std::uint64_t value = r.read_varuint();
  if (r.overrun() || value == 0 || value > kMaxVendorHaggleRounds) {
    return false;
  }
  round = std::uint32_t(value);
  return true;
}

inline bool line_fits(const VendorTradeLine& line) {
  return !line.row.empty() && line.row.size() <= kMaxRowIdBytes && line.units > 0;
}

inline bool write_lines(core::BitWriter& w, const std::vector<VendorTradeLine>& lines) {
  w.write_varuint(lines.size());
  for (const VendorTradeLine& line : lines) {
    if (!line_fits(line)) {
      return false;
    }
    w.write_varuint(line.row.size());
    w.write_bytes(reinterpret_cast<const std::uint8_t*>(line.row.data()), line.row.size());
    w.write_varuint(line.units);
  }
  return true;
}

inline bool read_lines(core::BitReader& r, std::vector<VendorTradeLine>& lines) {
  const std::uint64_t count = r.read_varuint();
  if (r.overrun() || count > kMaxVendorTradeLines) {
    return false;
  }
  lines.resize(std::size_t(count));
  for (VendorTradeLine& line : lines) {
    const std::uint64_t length = r.read_varuint();
    if (r.overrun() || length == 0 || length > kMaxRowIdBytes) {
      return false;
    }
    line.row.resize(std::size_t(length));
    for (char& c : line.row) {
      c = char(r.read_bits(8));
    }
    const std::uint64_t units = r.read_varuint();
    if (r.overrun() || units == 0 || units > 0xFFFFFFFFu) {
      return false;
    }
    line.units = std::uint32_t(units);
  }
  return true;
}

}  // namespace vendor_detail

// VENDOR_SHOW: [tag][varuint session][u8 flags: 1 open, 2 buys, 4 purse limited, 8 the keeper is
// a player, 16 the vendor haggles][varuint npc]
// [varuint revision][varuint purse][16 bytes money class][varuint shelf count][per row: the row as
// in INVENTORY_SHOW, varuint price][varuint buy count][per class: 16 bytes GUID, varuint price].
// Empty for a show that cannot travel: no session, too many rows, a row without an id, a price or
// purse above kMaxVendorPrice.
inline std::vector<std::uint8_t> encode_vendor_show(const VendorShow& show) {
  if (show.session == 0 || show.shelf.size() > kMaxVendorRows ||
      show.buy_prices.size() > kMaxVendorBuyRows || show.purse > kMaxVendorPrice) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kVendorShowTag, 8);
  w.write_varuint(show.session);
  const unsigned flags = (show.open ? 1 : 0) | (show.buys ? 2 : 0) | (show.purse_limited ? 4 : 0) |
                         (show.keeper_player ? 8 : 0) | (show.haggles ? 16 : 0);
  w.write_bits(flags, 8);
  w.write_varuint(show.npc);
  w.write_varuint(show.revision);
  w.write_varuint(show.purse);
  vendor_detail::write_guid(w, show.money);
  w.write_varuint(show.shelf.size());
  for (const VendorShelfRow& shelf_row : show.shelf) {
    const InventoryRowState& row = shelf_row.row;
    if (row.id.empty() || row.id.size() > kMaxRowIdBytes || row.amount == 0 ||
        shelf_row.price > kMaxVendorPrice) {
      return {};
    }
    inventory_detail::write_row(w, row, true);
    w.write_varuint(shelf_row.price);
  }
  w.write_varuint(show.buy_prices.size());
  for (const VendorBuyPrice& buy : show.buy_prices) {
    if (buy.price > kMaxVendorPrice) {
      return {};
    }
    vendor_detail::write_guid(w, buy.item);
    w.write_varuint(buy.price);
  }
  return w.bytes();
}

inline bool decode_vendor_show(const std::vector<std::uint8_t>& p, VendorShow& show) {
  if (p.empty() || p[0] != kVendorShowTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  VendorShow decoded;
  if (!vendor_detail::read_session(r, decoded.session)) {
    return false;
  }
  const std::uint64_t flags = r.read_bits(8);
  const std::uint64_t npc = r.read_varuint();
  decoded.revision = r.read_varuint();
  if (r.overrun() || flags > 31 || npc > 0xFFFFFFFFu ||
      !vendor_detail::read_price(r, decoded.purse)) {
    return false;
  }
  decoded.open = (flags & 1) != 0;
  decoded.buys = (flags & 2) != 0;
  decoded.purse_limited = (flags & 4) != 0;
  decoded.keeper_player = (flags & 8) != 0;
  decoded.haggles = (flags & 16) != 0;
  decoded.npc = std::uint32_t(npc);
  vendor_detail::read_guid(r, decoded.money);
  const std::uint64_t shelf = r.read_varuint();
  if (r.overrun() || shelf > kMaxVendorRows) {
    return false;
  }
  decoded.shelf.resize(std::size_t(shelf));
  for (VendorShelfRow& shelf_row : decoded.shelf) {
    if (!inventory_detail::read_row(r, shelf_row.row, true) ||
        !vendor_detail::read_price(r, shelf_row.price)) {
      return false;
    }
  }
  const std::uint64_t buys = r.read_varuint();
  if (r.overrun() || buys > kMaxVendorBuyRows) {
    return false;
  }
  decoded.buy_prices.resize(std::size_t(buys));
  for (VendorBuyPrice& buy : decoded.buy_prices) {
    vendor_detail::read_guid(r, buy.item);
    if (!vendor_detail::read_price(r, buy.price)) {
      return false;
    }
  }
  if (!vendor_detail::read_whole(r)) {
    return false;
  }
  show = std::move(decoded);
  return true;
}

// VENDOR_CLOSE: [tag][varuint session]; empty without a session.
inline std::vector<std::uint8_t> encode_vendor_close(const VendorClose& close) {
  if (close.session == 0) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kVendorCloseTag, 8);
  w.write_varuint(close.session);
  return w.bytes();
}

inline bool decode_vendor_close(const std::vector<std::uint8_t>& p, VendorClose& close) {
  if (p.empty() || p[0] != kVendorCloseTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  VendorClose decoded;
  if (!vendor_detail::read_session(r, decoded.session) || !vendor_detail::read_whole(r)) {
    return false;
  }
  close = decoded;
  return true;
}

// VENDOR_TRADE: [tag][varuint session][varuint revision][bought lines][sold lines]; lines are
// [varuint count][per line: varuint row id length, id bytes, varuint units]. Empty for a basket
// that cannot travel: no session, nothing in it, too many lines, a line without a row or units.
inline std::vector<std::uint8_t> encode_vendor_trade(const VendorTrade& trade) {
  if (trade.session == 0 || (trade.bought.empty() && trade.sold.empty()) ||
      trade.bought.size() > kMaxVendorTradeLines || trade.sold.size() > kMaxVendorTradeLines) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kVendorTradeTag, 8);
  w.write_varuint(trade.session);
  w.write_varuint(trade.revision);
  if (!vendor_detail::write_lines(w, trade.bought) || !vendor_detail::write_lines(w, trade.sold)) {
    return {};
  }
  return w.bytes();
}

inline bool decode_vendor_trade(const std::vector<std::uint8_t>& p, VendorTrade& trade) {
  if (p.empty() || p[0] != kVendorTradeTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  VendorTrade decoded;
  if (!vendor_detail::read_session(r, decoded.session)) {
    return false;
  }
  decoded.revision = r.read_varuint();
  if (r.overrun() || !vendor_detail::read_lines(r, decoded.bought) ||
      !vendor_detail::read_lines(r, decoded.sold) || !vendor_detail::read_whole(r) ||
      (decoded.bought.empty() && decoded.sold.empty())) {
    return false;
  }
  trade = std::move(decoded);
  return true;
}

// VENDOR_CLOSED: [tag][varuint session][u8 reason: 0, 3 or 4]; empty for one that cannot travel.
inline std::vector<std::uint8_t> encode_vendor_closed(const VendorClosed& closed) {
  const auto reason = std::uint8_t(closed.reason);
  if (closed.session == 0 || (reason != 0 && reason != 3 && reason != 4)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kVendorClosedTag, 8);
  w.write_varuint(closed.session);
  w.write_bits(reason, 8);
  return w.bytes();
}

inline bool decode_vendor_closed(const std::vector<std::uint8_t>& p, VendorClosed& closed) {
  if (p.empty() || p[0] != kVendorClosedTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  VendorClosed decoded;
  if (!vendor_detail::read_session(r, decoded.session)) {
    return false;
  }
  const std::uint64_t reason = r.read_bits(8);
  if (!vendor_detail::read_whole(r) || (reason != 0 && reason != 3 && reason != 4)) {
    return false;
  }
  decoded.reason = VendorClosedReason(reason);
  closed = decoded;
  return true;
}

// VENDOR_HAGGLE: [tag][varuint session][varuint revision][varuint round][price as
// write_signed_price][bought lines][sold lines], the lines as VENDOR_TRADE's. Empty for an offer
// that cannot travel: no session, no round, nothing in the basket, a price beyond kMaxVendorPrice.
inline std::vector<std::uint8_t> encode_vendor_haggle(const VendorHaggle& haggle) {
  if (haggle.session == 0 || haggle.round == 0 || haggle.round > kMaxVendorHaggleRounds ||
      (haggle.bought.empty() && haggle.sold.empty()) ||
      haggle.bought.size() > kMaxVendorTradeLines || haggle.sold.size() > kMaxVendorTradeLines) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kVendorHaggleTag, 8);
  w.write_varuint(haggle.session);
  w.write_varuint(haggle.revision);
  w.write_varuint(haggle.round);
  if (!vendor_detail::write_signed_price(w, haggle.offered) ||
      !vendor_detail::write_lines(w, haggle.bought) ||
      !vendor_detail::write_lines(w, haggle.sold)) {
    return {};
  }
  return w.bytes();
}

inline bool decode_vendor_haggle(const std::vector<std::uint8_t>& p, VendorHaggle& haggle) {
  if (p.empty() || p[0] != kVendorHaggleTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  VendorHaggle decoded;
  if (!vendor_detail::read_session(r, decoded.session)) {
    return false;
  }
  decoded.revision = r.read_varuint();
  if (r.overrun() || !vendor_detail::read_round(r, decoded.round) ||
      !vendor_detail::read_signed_price(r, decoded.offered) ||
      !vendor_detail::read_lines(r, decoded.bought) ||
      !vendor_detail::read_lines(r, decoded.sold) || !vendor_detail::read_whole(r) ||
      (decoded.bought.empty() && decoded.sold.empty())) {
    return false;
  }
  haggle = std::move(decoded);
  return true;
}

// VENDOR_HAGGLE_REPLY: [tag][varuint session][varuint round][u8 answer 1 .. 4][price as
// write_signed_price][u8 mood 0 .. kMaxVendorHaggleMood]. Empty for one that cannot travel.
inline std::vector<std::uint8_t> encode_vendor_haggle_reply(const VendorHaggleReply& reply) {
  const auto answer = std::uint8_t(reply.answer);
  if (reply.session == 0 || reply.round == 0 || reply.round > kMaxVendorHaggleRounds ||
      answer < 1 || answer > 4 || reply.mood > kMaxVendorHaggleMood) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kVendorHaggleReplyTag, 8);
  w.write_varuint(reply.session);
  w.write_varuint(reply.round);
  w.write_bits(answer, 8);
  if (!vendor_detail::write_signed_price(w, reply.price)) {
    return {};
  }
  w.write_bits(reply.mood, 8);
  return w.bytes();
}

inline bool decode_vendor_haggle_reply(const std::vector<std::uint8_t>& p,
                                       VendorHaggleReply& reply) {
  if (p.empty() || p[0] != kVendorHaggleReplyTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  VendorHaggleReply decoded;
  if (!vendor_detail::read_session(r, decoded.session) ||
      !vendor_detail::read_round(r, decoded.round)) {
    return false;
  }
  const std::uint64_t answer = r.read_bits(8);
  if (r.overrun() || answer < 1 || answer > 4 ||
      !vendor_detail::read_signed_price(r, decoded.price)) {
    return false;
  }
  const std::uint64_t mood = r.read_bits(8);
  if (!vendor_detail::read_whole(r) || mood > kMaxVendorHaggleMood) {
    return false;
  }
  decoded.answer = VendorHaggleAnswer(answer);
  decoded.mood = std::uint8_t(mood);
  reply = decoded;
  return true;
}

}  // namespace kcd2s::integration
