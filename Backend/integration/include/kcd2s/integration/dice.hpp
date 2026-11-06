#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "kcd2s/core/bits.hpp"
#include "kcd2s/integration/guid.hpp"

// Dice matches between two players, played at the game's own dice table. On the
// control channel (0).
//   DICE_STATE : server -> each player's own client, the match as that player sees it:
//                `Start`  the match begins: the other player, the table's behavior entity (its
//                         level EntityGuid, as the game starts its dice minigame there), this
//                         player's seat (0 the table's seat1_SO, 1 its seat2_SO), whether this
//                         player throws first, the target score and both scoreboard names (own
//                         first; empty for the game's own label), both players' dice and both
//                         players' badges (the badge item's class, its effect and tier; none
//                         without).
//                `Roll`   a throw the server dealt: whose (this player's or the other's), the seven
//                         faces (the seventh a Might badge's die), which dice it threw and whether
//                         it busted.
//                `Move`   the other player kept dice (a mask of the seven) and threw the rest again
//                         (Continue) or banked the turn (Pass, BadgePass with their Doppelganger or
//                         Warlord badge), the dice the other player has selected so far in their
//                         turn (Select, any mask, none included), used their badge on dice of their
//                         throw (Badge: the dice, none for a Might badge) or chose after a bust
//                         with their Resurrection badge (BustChoice: held 0 bank, 1 throw again, 2
//                         bust).
//                `End`    the match is over: why and how it went for this player.
//                `Names`  the scoreboard names changed (own first; empty for the game's own).
//   DICE_MOVE  : the player's own client -> server: the player kept dice and threw on (Continue)
//                or banked (Pass), gave up (left the table) or could not play the match (Failed);
//                the player's game throws now and waits for the server's throw (Throw); the dice
//                the player has selected so far (Select), shown to the other player as they go;
//                the player used a dice table in their game ("Play dice", TableUse, match 0 when
//                the player plays none); the player banked with their badge (BadgePass), used it
//                on dice of their throw (Badge) or chose after a bust (BustChoice), as in Move.
// The server deals a throw when the thrower's game throws (Throw), so both games show it as it
// happens.
//
// Messages are byte-packed with varints (kcd2s/core/bits.hpp).

namespace kcd2s::integration {

inline constexpr std::uint8_t kDiceStateTag = 133;
inline constexpr std::uint8_t kDiceMoveTag = 134;
// Moves a client may send in a second, and the longest name.
inline constexpr unsigned kMaxDiceMovesPerSecond = 8;
inline constexpr std::size_t kMaxDiceNameBytes = 64;
// The longest mesh file a die of a kind names.
inline constexpr std::size_t kMaxDiceMeshBytes = 128;
// The joker side of the game's joker dice, as a face (the game counts it as side value 6).
inline constexpr std::uint8_t kDiceJoker = 7;
inline constexpr std::uint32_t kMaxDiceTarget = 100000;
inline constexpr std::uint8_t kAllDice = 0x3F;
// The seven dice of a turn with a Might badge's die.
inline constexpr std::uint8_t kDiceCount = 7;
inline constexpr std::uint8_t kSevenDice = 0x7F;
// A Resurrection badge's choices after a bust: bank, throw again, take the bust.
inline constexpr std::uint8_t kDiceBustChoices = 3;
// The game's badge effects (dice_badge_subtype: 0 Headstart to 12 Balatro) and tiers
// (dice_badge_type: 0 tin, 1 silver, 2 gold).
inline constexpr std::uint8_t kDiceBadgeEffects = 13;
inline constexpr std::uint8_t kDiceBadgeTiers = 3;

// A player's badge in a match: the badge item's class, its effect and its tier. No badge has the
// zero class, effect 0 and tier 0.
struct DiceBadge {
  Guid item{};
  std::uint8_t effect = 0;
  std::uint8_t tier = 0;

  bool present() const {
    return item != Guid{};
  }
  bool operator==(const DiceBadge&) const = default;
};

enum class DiceEnd : std::uint8_t {
  Won = 1,
  GaveUp = 2,
  Left = 3,
  TimedOut = 4,
  Failed = 5,
  Stopped = 6,
};

enum class DiceResult : std::uint8_t { Lost = 0, Won = 1, NoWinner = 2 };

struct DiceState {
  enum class Kind : std::uint8_t { Start = 1, Roll = 2, Move = 3, End = 4, Names = 5, Sit = 6 };
  enum class Move : std::uint8_t {
    Continue = 1,
    Pass = 2,
    Select = 3,
    Badge = 4,
    BadgePass = 5,
    BustChoice = 6
  };
  Kind kind = Kind::Start;
  std::uint32_t match = 0;
  // Start
  std::uint32_t opponent = 0;
  // Start and Sit: the table's behavior and the player's seat at it (Sit: no match, 0).
  std::uint64_t behavior = 0;
  std::uint8_t seat = 0;
  bool first = false;
  std::uint32_t target = 0;
  // Start and Names
  std::string own_name;
  std::string opponent_name;
  // Start: the meshes the player's own and the other player's six dice show with, none for the
  // game's plain dice.
  std::vector<std::string> own_dice;
  std::vector<std::string> opponent_dice;
  // Start: each of those dice's six side values (0 to 5 the faces 1 to 6, 6 the joker), none with
  // the meshes.
  std::vector<std::array<std::uint8_t, 6>> own_sides;
  std::vector<std::array<std::uint8_t, 6>> opponent_sides;
  // Start: the player's own and the other player's badge.
  DiceBadge own_badge;
  DiceBadge opponent_badge;
  // Roll
  bool mine = false;
  std::array<std::uint8_t, kDiceCount> faces{};
  std::uint8_t rolled = 0;
  bool bust = false;
  // Move
  Move move = Move::Continue;
  std::uint8_t held = 0;
  // End
  DiceEnd reason = DiceEnd::Won;
  DiceResult result = DiceResult::NoWinner;
  bool operator==(const DiceState&) const = default;
};

struct DiceMove {
  enum class Kind : std::uint8_t {
    Continue = 1,
    Pass = 2,
    GaveUp = 3,
    Failed = 4,
    Throw = 5,
    Select = 6,
    TableUse = 7,
    BadgePass = 8,
    Badge = 9,
    BustChoice = 10
  };
  std::uint32_t match = 0;
  Kind kind = Kind::Continue;
  std::uint8_t held = 0;
  bool operator==(const DiceMove&) const = default;
};

namespace dice_detail {

// At most kMaxDiceNameBytes, no control characters.
inline bool name_valid(const std::string& name) {
  if (name.size() > kMaxDiceNameBytes) {
    return false;
  }
  for (const char c : name) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7F) {
      return false;
    }
  }
  return true;
}

inline void write_name(core::BitWriter& w, const std::string& name) {
  w.write_varuint(name.size());
  w.write_bytes(reinterpret_cast<const std::uint8_t*>(name.data()), name.size());
}

inline bool read_name(core::BitReader& r, std::string& name) {
  const std::uint64_t length = r.read_varuint();
  if (r.overrun() || length > kMaxDiceNameBytes) {
    return false;
  }
  name.resize(length);
  for (char& c : name) {
    c = char(r.read_bits(8));
  }
  return !r.overrun() && name_valid(name);
}

// A die's mesh file: letters, digits and _ / . - only, at most kMaxDiceMeshBytes.
inline bool mesh_valid(const std::string& mesh) {
  if (mesh.empty() || mesh.size() > kMaxDiceMeshBytes) {
    return false;
  }
  for (const char c : mesh) {
    const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (!letter && c != '_' && c != '/' && c != '.' && c != '-') {
      return false;
    }
  }
  return true;
}

// None (the game's plain dice) or six meshes.
inline bool dice_valid(const std::vector<std::string>& dice) {
  if (dice.empty()) {
    return true;
  }
  if (dice.size() != 6) {
    return false;
  }
  for (const std::string& mesh : dice) {
    if (!mesh_valid(mesh)) {
      return false;
    }
  }
  return true;
}

// As many sides as meshes, each side a value 0 to 6.
inline bool sides_valid(const std::vector<std::string>& dice,
                        const std::vector<std::array<std::uint8_t, 6>>& sides) {
  if (sides.size() != dice.size()) {
    return false;
  }
  for (const auto& die : sides) {
    for (const std::uint8_t side : die) {
      if (side > kDiceJoker - 1) {
        return false;
      }
    }
  }
  return true;
}

inline void write_sides(core::BitWriter& w, const std::vector<std::array<std::uint8_t, 6>>& sides) {
  for (const auto& die : sides) {
    for (const std::uint8_t side : die) {
      w.write_bits(side, 8);
    }
  }
}

inline bool read_sides(core::BitReader& r, std::size_t count,
                       std::vector<std::array<std::uint8_t, 6>>& sides) {
  sides.assign(count, {});
  for (auto& die : sides) {
    for (std::uint8_t& side : die) {
      side = std::uint8_t(r.read_bits(8));
    }
  }
  return !r.overrun();
}

// No badge, or an effect and a tier of the game's.
inline bool badge_valid(const DiceBadge& badge) {
  if (!badge.present()) {
    return badge.effect == 0 && badge.tier == 0;
  }
  return badge.effect < kDiceBadgeEffects && badge.tier < kDiceBadgeTiers;
}

inline void write_badge(core::BitWriter& w, const DiceBadge& badge) {
  w.write_bits(badge.present() ? 1 : 0, 8);
  if (!badge.present()) {
    return;
  }
  w.write_bytes(badge.item.data(), badge.item.size());
  w.write_bits(badge.effect, 8);
  w.write_bits(badge.tier, 8);
}

inline bool read_badge(core::BitReader& r, DiceBadge& badge) {
  badge = DiceBadge{};
  const std::uint64_t present = r.read_bits(8);
  if (r.overrun() || present > 1) {
    return false;
  }
  if (present == 0) {
    return true;
  }
  for (std::uint8_t& byte : badge.item) {
    byte = std::uint8_t(r.read_bits(8));
  }
  badge.effect = std::uint8_t(r.read_bits(8));
  badge.tier = std::uint8_t(r.read_bits(8));
  return !r.overrun() && badge.present() && badge_valid(badge);
}

inline void write_dice(core::BitWriter& w, const std::vector<std::string>& dice) {
  w.write_bits(dice.size(), 8);
  for (const std::string& mesh : dice) {
    w.write_varuint(mesh.size());
    w.write_bytes(reinterpret_cast<const std::uint8_t*>(mesh.data()), mesh.size());
  }
}

inline bool read_dice(core::BitReader& r, std::vector<std::string>& dice) {
  const std::uint64_t count = r.read_bits(8);
  if (r.overrun() || (count != 0 && count != 6)) {
    return false;
  }
  dice.assign(count, std::string());
  for (std::string& mesh : dice) {
    const std::uint64_t length = r.read_varuint();
    if (r.overrun() || length > kMaxDiceMeshBytes) {
      return false;
    }
    mesh.resize(length);
    for (char& c : mesh) {
      c = char(r.read_bits(8));
    }
  }
  return !r.overrun() && dice_valid(dice);
}

// A face: 1 to 6, or kDiceJoker the joker side of the game's joker dice; 0 for a die not thrown.
inline bool faces_valid(const std::array<std::uint8_t, kDiceCount>& faces, std::uint8_t rolled) {
  for (std::size_t die = 0; die < faces.size(); ++die) {
    const bool thrown = (rolled & (1u << die)) != 0;
    if (thrown && (faces[die] < 1 || faces[die] > kDiceJoker)) {
      return false;
    }
    if (faces[die] > kDiceJoker) {
      return false;
    }
  }
  return true;
}

// A move's dice: any for Select and Badge (none for a Might badge), some for a continue or a pass,
// a choice for BustChoice.
inline bool move_valid(DiceState::Move move, std::uint8_t held) {
  switch (move) {
    case DiceState::Move::Select:
    case DiceState::Move::Badge:
      return (held & ~kSevenDice) == 0;
    case DiceState::Move::Continue:
    case DiceState::Move::Pass:
    case DiceState::Move::BadgePass:
      return held != 0 && (held & ~kSevenDice) == 0;
    case DiceState::Move::BustChoice:
      return held < kDiceBustChoices;
  }
  return false;
}

inline bool state_valid(const DiceState& state) {
  switch (state.kind) {
    case DiceState::Kind::Start:
      return state.seat <= 1 && state.target >= 1 && state.target <= kMaxDiceTarget &&
             name_valid(state.own_name) && name_valid(state.opponent_name) &&
             dice_valid(state.own_dice) && dice_valid(state.opponent_dice) &&
             sides_valid(state.own_dice, state.own_sides) &&
             sides_valid(state.opponent_dice, state.opponent_sides) &&
             badge_valid(state.own_badge) && badge_valid(state.opponent_badge);
    case DiceState::Kind::Names:
      return name_valid(state.own_name) && name_valid(state.opponent_name);
    case DiceState::Kind::Sit:
      return state.match == 0 && state.behavior != 0 && state.seat <= 1;
    case DiceState::Kind::Roll:
      return state.rolled != 0 && (state.rolled & ~kSevenDice) == 0 &&
             faces_valid(state.faces, state.rolled);
    case DiceState::Kind::Move:
      return move_valid(state.move, state.held);
    case DiceState::Kind::End:
      return state.reason >= DiceEnd::Won && state.reason <= DiceEnd::Stopped &&
             state.result <= DiceResult::NoWinner;
  }
  return false;
}

}  // namespace dice_detail

// DICE_STATE: [tag][u8 kind][varuint match] then
//   Start: [varuint opponent][u64 behavior][u8 seat][u8 first][varuint target][name own]
//          [name opponent][dice own][dice opponent][sides own][sides opponent][badge own]
//          [badge opponent]
//   Roll:  [u8 mine][u8 x7 faces][u8 rolled][u8 bust]
//   Move:  [u8 move][u8 held]
//   End:   [u8 reason][u8 result]
//   Names: [name own][name opponent]
//   Sit:   [u64 behavior][u8 seat] (match 0: the player sits down at the table and waits)
// a name being [varuint n][n bytes], dice [u8 count, 0 or 6] then count meshes as names, sides six
// u8 per die of those dice, a badge [u8 present] then, present, [16 bytes class][u8 effect]
// [u8 tier]. Empty for a state that cannot travel.
inline std::vector<std::uint8_t> encode_dice_state(const DiceState& state) {
  if (!dice_detail::state_valid(state)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kDiceStateTag, 8);
  w.write_bits(std::uint8_t(state.kind), 8);
  w.write_varuint(state.match);
  switch (state.kind) {
    case DiceState::Kind::Start:
      w.write_varuint(state.opponent);
      w.write_bits(state.behavior, 64);
      w.write_bits(state.seat, 8);
      w.write_bits(state.first ? 1 : 0, 8);
      w.write_varuint(state.target);
      dice_detail::write_name(w, state.own_name);
      dice_detail::write_name(w, state.opponent_name);
      dice_detail::write_dice(w, state.own_dice);
      dice_detail::write_dice(w, state.opponent_dice);
      dice_detail::write_sides(w, state.own_sides);
      dice_detail::write_sides(w, state.opponent_sides);
      dice_detail::write_badge(w, state.own_badge);
      dice_detail::write_badge(w, state.opponent_badge);
      break;
    case DiceState::Kind::Roll:
      w.write_bits(state.mine ? 1 : 0, 8);
      for (const std::uint8_t face : state.faces) {
        w.write_bits(face, 8);
      }
      w.write_bits(state.rolled, 8);
      w.write_bits(state.bust ? 1 : 0, 8);
      break;
    case DiceState::Kind::Move:
      w.write_bits(std::uint8_t(state.move), 8);
      w.write_bits(state.held, 8);
      break;
    case DiceState::Kind::End:
      w.write_bits(std::uint8_t(state.reason), 8);
      w.write_bits(std::uint8_t(state.result), 8);
      break;
    case DiceState::Kind::Names:
      dice_detail::write_name(w, state.own_name);
      dice_detail::write_name(w, state.opponent_name);
      break;
    case DiceState::Kind::Sit:
      w.write_bits(state.behavior, 64);
      w.write_bits(state.seat, 8);
      break;
  }
  return w.bytes();
}

inline bool decode_dice_state(const std::vector<std::uint8_t>& p, DiceState& state) {
  if (p.empty() || p[0] != kDiceStateTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  DiceState decoded;
  const std::uint64_t kind = r.read_bits(8);
  const std::uint64_t match = r.read_varuint();
  if (r.overrun() || kind < 1 || kind > 6 || match > 0xFFFFFFFFu) {
    return false;
  }
  decoded.kind = DiceState::Kind(kind);
  decoded.match = std::uint32_t(match);
  switch (decoded.kind) {
    case DiceState::Kind::Start: {
      const std::uint64_t opponent = r.read_varuint();
      decoded.behavior = r.read_bits(64);
      const std::uint64_t seat = r.read_bits(8);
      const std::uint64_t first = r.read_bits(8);
      const std::uint64_t target = r.read_varuint();
      if (r.overrun() || opponent > 0xFFFFFFFFu || seat > 1 || first > 1 ||
          target > kMaxDiceTarget) {
        return false;
      }
      decoded.opponent = std::uint32_t(opponent);
      decoded.seat = std::uint8_t(seat);
      decoded.first = first == 1;
      decoded.target = std::uint32_t(target);
      if (!dice_detail::read_name(r, decoded.own_name) ||
          !dice_detail::read_name(r, decoded.opponent_name) ||
          !dice_detail::read_dice(r, decoded.own_dice) ||
          !dice_detail::read_dice(r, decoded.opponent_dice) ||
          !dice_detail::read_sides(r, decoded.own_dice.size(), decoded.own_sides) ||
          !dice_detail::read_sides(r, decoded.opponent_dice.size(), decoded.opponent_sides) ||
          !dice_detail::sides_valid(decoded.own_dice, decoded.own_sides) ||
          !dice_detail::sides_valid(decoded.opponent_dice, decoded.opponent_sides) ||
          !dice_detail::read_badge(r, decoded.own_badge) ||
          !dice_detail::read_badge(r, decoded.opponent_badge)) {
        return false;
      }
      break;
    }
    case DiceState::Kind::Roll: {
      const std::uint64_t mine = r.read_bits(8);
      for (std::uint8_t& face : decoded.faces) {
        face = std::uint8_t(r.read_bits(8));
      }
      const std::uint64_t rolled = r.read_bits(8);
      const std::uint64_t bust = r.read_bits(8);
      if (r.overrun() || mine > 1 || bust > 1) {
        return false;
      }
      decoded.mine = mine == 1;
      decoded.rolled = std::uint8_t(rolled);
      decoded.bust = bust == 1;
      break;
    }
    case DiceState::Kind::Move: {
      const std::uint64_t move = r.read_bits(8);
      decoded.held = std::uint8_t(r.read_bits(8));
      decoded.move = DiceState::Move(move);
      break;
    }
    case DiceState::Kind::End: {
      decoded.reason = DiceEnd(r.read_bits(8));
      decoded.result = DiceResult(r.read_bits(8));
      break;
    }
    case DiceState::Kind::Names: {
      if (!dice_detail::read_name(r, decoded.own_name) ||
          !dice_detail::read_name(r, decoded.opponent_name)) {
        return false;
      }
      break;
    }
    case DiceState::Kind::Sit: {
      decoded.behavior = r.read_bits(64);
      decoded.seat = std::uint8_t(r.read_bits(8));
      break;
    }
  }
  if (r.overrun() || r.bits_remaining() != 0 || !dice_detail::state_valid(decoded)) {
    return false;
  }
  state = std::move(decoded);
  return true;
}

// DICE_MOVE: [tag][varuint match][u8 kind][u8 held]. Empty for a move that cannot travel.
inline std::vector<std::uint8_t> encode_dice_move(const DiceMove& move) {
  if (move.kind < DiceMove::Kind::Continue || move.kind > DiceMove::Kind::BustChoice ||
      (move.held & ~kSevenDice) != 0) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kDiceMoveTag, 8);
  w.write_varuint(move.match);
  w.write_bits(std::uint8_t(move.kind), 8);
  w.write_bits(move.held, 8);
  return w.bytes();
}

inline bool decode_dice_move(const std::vector<std::uint8_t>& p, DiceMove& move) {
  if (p.empty() || p[0] != kDiceMoveTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  const std::uint64_t match = r.read_varuint();
  const std::uint64_t kind = r.read_bits(8);
  const std::uint64_t held = r.read_bits(8);
  if (r.overrun() || r.bits_remaining() != 0 || match > 0xFFFFFFFFu || kind < 1 || kind > 10 ||
      (held & ~std::uint64_t(kSevenDice)) != 0) {
    return false;
  }
  move.match = std::uint32_t(match);
  move.kind = DiceMove::Kind(kind);
  move.held = std::uint8_t(held);
  return true;
}

}  // namespace kcd2s::integration
