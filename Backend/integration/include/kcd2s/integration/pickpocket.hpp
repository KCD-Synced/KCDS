#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "kcd2s/core/bits.hpp"
#include "kcd2s/integration/inventory.hpp"

// Robbing another player with the game's own pickpocketing. The
// thief's game runs the minigame against the victim's replica; the server decides whether a
// theft may start and hears how it ended.
//
//   PICKPOCKET      : the thief's client -> server, on the control channel (0): a request to
//                     start robbing a player (the game's Rob entry was held) or how a theft ended
//                     (the game's own result: the victim noticed, the thief was seen, it ended).
//   PICKPOCKET_RULE : server -> a player's client: whether its game offers Rob at all, the answer
//                     to its request, or (to the victim) that a thief was caught at its pockets.
//   PICKPOCKET_POCKETS : server -> the thief's client, before an allowing answer: what the victim
//                     carries and does not wear (rows: class, amount, quality, health), which the
//                     victim's replica holds for the game's own loot screen.
namespace kcd2s::integration {

inline constexpr std::uint8_t kPickpocketTag = 44;
inline constexpr std::uint8_t kPickpocketRuleTag = 45;
inline constexpr std::uint8_t kPickpocketPocketsTag = 46;
inline constexpr std::size_t kMaxPickpocketRows = 64;
// The longest notice text a rule carries, in bytes.
inline constexpr std::size_t kMaxPickpocketNoticeBytes = 160;

struct PickpocketReport {
  enum class Kind : std::uint8_t {
    Request = 1,   // the thief wants to rob the player
    Caught = 2,    // the victim noticed the thief (the game's "detected by victim")
    Seen = 3,      // someone else saw the thief
    Finished = 4,  // the theft ended otherwise (the loot screen closed, lost, interrupted)
  };
  Kind kind = Kind::Request;
  std::uint32_t victim = 0;  // the victim's network entity id

  friend bool operator==(const PickpocketReport& a, const PickpocketReport& b) {
    return a.kind == b.kind && a.victim == b.victim;
  }
};

struct PickpocketRule {
  enum class Kind : std::uint8_t {
    Enabled = 1,      // whether the game offers Rob on other players (flag)
    Answer = 2,       // the answer to a request against player (flag: allowed)
    Noticed = 3,      // to the victim: player was caught at its pockets
    StolenMarks = 4,  // whether the game marks items as stolen at all (flag)
  };
  Kind kind = Kind::Enabled;
  bool flag = true;
  std::uint32_t player = 0;
  std::string text;  // Noticed: what the victim's game shows

  friend bool operator==(const PickpocketRule& a, const PickpocketRule& b) {
    return a.kind == b.kind && a.flag == b.flag && a.player == b.player && a.text == b.text;
  }
};

namespace pickpocket_detail {

inline bool valid_report_kind(std::uint64_t kind) {
  return kind >= 1 && kind <= 4;
}

inline bool valid_rule_kind(std::uint64_t kind) {
  return kind >= 1 && kind <= 4;
}

}  // namespace pickpocket_detail

// PICKPOCKET: [tag][u8 kind][varuint victim]; empty for an unknown kind or no victim.
inline std::vector<std::uint8_t> encode_pickpocket_report(const PickpocketReport& report) {
  if (!pickpocket_detail::valid_report_kind(std::uint8_t(report.kind)) || report.victim == 0) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kPickpocketTag, 8);
  w.write_bits(std::uint8_t(report.kind), 8);
  w.write_varuint(report.victim);
  return w.bytes();
}

inline bool decode_pickpocket_report(const std::vector<std::uint8_t>& p, PickpocketReport& report) {
  if (p.empty() || p[0] != kPickpocketTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  const auto kind = r.read_bits(8);
  const std::uint64_t victim = r.read_varuint();
  if (r.overrun() || r.bits_remaining() != 0 || !pickpocket_detail::valid_report_kind(kind) ||
      victim == 0 || victim > 0xFFFFFFFFu) {
    return false;
  }
  report.kind = PickpocketReport::Kind(kind);
  report.victim = std::uint32_t(victim);
  return true;
}

// PICKPOCKET_RULE: [tag][u8 kind][u8 flag][varuint player][varuint length][text]; empty for an
// unknown kind or a text too long.
inline std::vector<std::uint8_t> encode_pickpocket_rule(const PickpocketRule& rule) {
  if (!pickpocket_detail::valid_rule_kind(std::uint8_t(rule.kind)) ||
      rule.text.size() > kMaxPickpocketNoticeBytes) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kPickpocketRuleTag, 8);
  w.write_bits(std::uint8_t(rule.kind), 8);
  w.write_bits(rule.flag ? 1 : 0, 8);
  w.write_varuint(rule.player);
  w.write_varuint(rule.text.size());
  for (const char c : rule.text) {
    w.write_bits(std::uint8_t(c), 8);
  }
  return w.bytes();
}

inline bool decode_pickpocket_rule(const std::vector<std::uint8_t>& p, PickpocketRule& rule) {
  if (p.empty() || p[0] != kPickpocketRuleTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  const auto kind = r.read_bits(8);
  const auto flag = r.read_bits(8);
  const std::uint64_t player = r.read_varuint();
  const std::uint64_t length = r.read_varuint();
  if (r.overrun() || length > kMaxPickpocketNoticeBytes) {
    return false;
  }
  std::string text;
  for (std::uint64_t i = 0; i < length; ++i) {
    text.push_back(char(r.read_bits(8)));
  }
  if (r.overrun() || r.bits_remaining() != 0 || !pickpocket_detail::valid_rule_kind(kind) ||
      flag > 1 || player > 0xFFFFFFFFu) {
    return false;
  }
  rule.kind = PickpocketRule::Kind(kind);
  rule.flag = flag == 1;
  rule.player = std::uint32_t(player);
  rule.text = std::move(text);
  return true;
}

// What a victim carries, for the thief's loot screen.
struct PickpocketPockets {
  std::uint32_t victim = 0;
  std::vector<InventoryRowState> rows;  // ids and equipped are not sent

  friend bool operator==(const PickpocketPockets& a, const PickpocketPockets& b) {
    return a.victim == b.victim && a.rows == b.rows;
  }
};

// PICKPOCKET_POCKETS: [tag][varuint victim][varuint count] then per row [16 bytes class]
// [varuint amount][u8 quality][f32 health]; empty for no victim, too many rows or an empty row.
inline std::vector<std::uint8_t> encode_pickpocket_pockets(const PickpocketPockets& pockets) {
  if (pockets.victim == 0 || pockets.rows.size() > kMaxPickpocketRows) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kPickpocketPocketsTag, 8);
  w.write_varuint(pockets.victim);
  w.write_varuint(pockets.rows.size());
  for (const InventoryRowState& row : pockets.rows) {
    if (row.amount == 0) {
      return {};
    }
    for (const std::uint8_t byte : row.item) {
      w.write_bits(byte, 8);
    }
    w.write_varuint(row.amount);
    w.write_bits(row.quality, 8);
    std::uint32_t health_bits = 0;
    std::memcpy(&health_bits, &row.health, sizeof(health_bits));
    w.write_bits(health_bits, 32);
  }
  return w.bytes();
}

inline bool decode_pickpocket_pockets(const std::vector<std::uint8_t>& p,
                                      PickpocketPockets& pockets) {
  if (p.empty() || p[0] != kPickpocketPocketsTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  PickpocketPockets decoded;
  const std::uint64_t victim = r.read_varuint();
  const std::uint64_t count = r.read_varuint();
  if (r.overrun() || victim == 0 || victim > 0xFFFFFFFFu || count > kMaxPickpocketRows) {
    return false;
  }
  decoded.victim = std::uint32_t(victim);
  for (std::uint64_t i = 0; i < count; ++i) {
    InventoryRowState row;
    for (std::uint8_t& byte : row.item) {
      byte = std::uint8_t(r.read_bits(8));
    }
    const std::uint64_t amount = r.read_varuint();
    row.quality = std::uint8_t(r.read_bits(8));
    const auto health_bits = std::uint32_t(r.read_bits(32));
    std::memcpy(&row.health, &health_bits, sizeof(row.health));
    if (r.overrun() || amount == 0 || amount > 0xFFFFFFFFu || !std::isfinite(row.health)) {
      return false;
    }
    row.amount = std::uint32_t(amount);
    decoded.rows.push_back(row);
  }
  if (r.bits_remaining() != 0) {
    return false;
  }
  pockets = std::move(decoded);
  return true;
}

}  // namespace kcd2s::integration
