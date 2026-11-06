#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "kcd2s/integration/guid.hpp"

// A player's progression: what the player's own game holds, reported to
// the server, which keeps it, checks it and gives the resources Player.levels, perks,
// progression and trackOf. On the control channel (0); tags 1-16 are taken by control.hpp,
// world_config.hpp, knockout.hpp, appearance.hpp, player_identity.hpp, player_stats.hpp and
// movement_mode.hpp.
//   PROGRESSION       : the player's own client -> server, the whole snapshot whenever it
//                       changed: every track's level, XP towards the next level and unspent perk
//                       points, the main level's unspent points, and every perk it owns.
//   PROGRESSION_ORDER : server -> the player's own client, one of the server's progression
//                       orders (Player.addXp, setLevel, setXp, addPerk, removePerk,
//                       addPerkPoints, resetPerks, loadProgression, Progression.setGameXp),
//                       applied through the game's own paths; setTrack and an exact restore
//                       write levels and XP directly.
//   XP_GAINING        : the player's own client -> server, XP its game produced and held back:
//                       the server decides what is granted (playerXpGaining, rates, budgets,
//                       caps) and grants it with an AddXp order.
//   PERK_LEARNING     : the player's own client -> server, a perk the player confirmed on the
//                       perk screen and the game allows, held back: the server decides
//                       (playerPerkLearning, its block list) and lets it be learnt with a
//                       LearnPerk order.
namespace kcd2s::integration {

inline constexpr std::uint8_t kProgressionTag = 17;
inline constexpr std::uint8_t kProgressionOrderTag = 18;
inline constexpr std::uint8_t kXpGainingTag = 19;
inline constexpr std::uint8_t kPerkLearningTag = 20;

// A track as the game numbers it: a skill or a stat, and its id in the game's skill or stat
// table. The order is Progression.tracks()'s (Backend/scripting/runtimes/js/progression).
struct TrackId {
  bool stat = false;
  std::uint8_t id = 0;
};
inline constexpr std::array<TrackId, 20> kTracks = {{
    {false, 0},   // stealth
    {false, 1},   // horse_riding
    {false, 2},   // fencing
    {false, 4},   // thievery
    {false, 6},   // alchemy
    {false, 8},   // craftsmanship
    {false, 13},  // drinking
    {false, 14},  // survival
    {false, 16},  // weapon_sword
    {false, 17},  // heavy_weapons
    {false, 19},  // marksmanship
    {false, 23},  // weapon_large
    {false, 24},  // weapon_unarmed
    {false, 26},  // scholarship
    {false, 34},  // houndmaster
    {true, 0},    // strength
    {true, 1},    // agility
    {true, 2},    // vitality
    {true, 3},    // speech
    {true, 9},    // prestige
}};
// The highest level the game lets a track reach.
inline constexpr std::uint8_t kLevelCap = 30;
// XP is kept as the game keeps it: fixed point, this many units per XP.
inline constexpr std::uint32_t kXpUnits = 128;
// A character owns a few hundred perks at most; the game's tables carry 945.
inline constexpr std::size_t kMaxPerks = 1024;

struct TrackState {
  std::uint8_t level = 0;
  std::uint32_t xp = 0;  // towards the next level, in kXpUnits per XP
  std::uint16_t points = 0;

  friend bool operator==(const TrackState& a, const TrackState& b) {
    return a.level == b.level && a.xp == b.xp && a.points == b.points;
  }
};

// A perk's GUID as the game keeps it.
using PerkId = Guid;

struct Progression {
  std::array<TrackState, kTracks.size()> tracks{};
  std::uint16_t main_points = 0;
  std::vector<PerkId> perks;  // sorted, no duplicates

  friend bool operator==(const Progression& a, const Progression& b) {
    return a.tracks == b.tracks && a.main_points == b.main_points && a.perks == b.perks;
  }
};

// The perks sorted and without duplicates, as PROGRESSION carries them.
inline void normalize_perks(std::vector<PerkId>& perks) {
  std::sort(perks.begin(), perks.end());
  perks.erase(std::unique(perks.begin(), perks.end()), perks.end());
}

// The GUID's text as the game's tables write it (guid_text).
inline std::string perk_id_text(const PerkId& id) {
  return guid_text(id);
}

// The GUID of its text, either case (parse_guid); false for anything else.
inline bool parse_perk_id(const std::string& text, PerkId& id) {
  return parse_guid(text, id);
}

namespace detail {

inline void put_u16(std::vector<std::uint8_t>& p, std::uint16_t value) {
  p.push_back(std::uint8_t(value));
  p.push_back(std::uint8_t(value >> 8));
}

inline void put_u32(std::vector<std::uint8_t>& p, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    p.push_back(std::uint8_t(value >> shift));
  }
}

inline std::uint16_t get_u16(const std::vector<std::uint8_t>& p, std::size_t at) {
  return std::uint16_t(p[at] | p[at + 1] << 8);
}

inline std::uint32_t get_u32(const std::vector<std::uint8_t>& p, std::size_t at) {
  return std::uint32_t(p[at]) | std::uint32_t(p[at + 1]) << 8 | std::uint32_t(p[at + 2]) << 16 |
         std::uint32_t(p[at + 3]) << 24;
}

inline constexpr std::size_t kTrackBytes = 1 + 4 + 2;

}  // namespace detail

// PROGRESSION: [tag][per track: u8 level, u32 xp, u16 points][u16 main points][u16 perk count]
// [16 bytes per perk]. Empty when the snapshot cannot travel (a level past the cap, too many
// perks).
inline std::vector<std::uint8_t> encode_progression(const Progression& progression) {
  if (progression.perks.size() > kMaxPerks) {
    return {};
  }
  std::vector<std::uint8_t> p{kProgressionTag};
  for (const TrackState& track : progression.tracks) {
    if (track.level > kLevelCap) {
      return {};
    }
    p.push_back(track.level);
    detail::put_u32(p, track.xp);
    detail::put_u16(p, track.points);
  }
  detail::put_u16(p, progression.main_points);
  detail::put_u16(p, std::uint16_t(progression.perks.size()));
  for (const PerkId& perk : progression.perks) {
    p.insert(p.end(), perk.begin(), perk.end());
  }
  return p;
}

inline bool decode_progression(const std::vector<std::uint8_t>& p, Progression& progression) {
  constexpr std::size_t kHeader = 1 + kTracks.size() * detail::kTrackBytes + 2 + 2;
  if (p.size() < kHeader || p[0] != kProgressionTag) {
    return false;
  }
  Progression decoded;
  std::size_t at = 1;
  for (TrackState& track : decoded.tracks) {
    track.level = p[at];
    track.xp = detail::get_u32(p, at + 1);
    track.points = detail::get_u16(p, at + 5);
    if (track.level > kLevelCap) {
      return false;
    }
    at += detail::kTrackBytes;
  }
  decoded.main_points = detail::get_u16(p, at);
  const std::size_t count = detail::get_u16(p, at + 2);
  at += 4;
  if (count > kMaxPerks || p.size() != at + 16 * count) {
    return false;
  }
  decoded.perks.resize(count);
  for (PerkId& perk : decoded.perks) {
    std::copy(p.begin() + std::ptrdiff_t(at), p.begin() + std::ptrdiff_t(at + 16), perk.begin());
    at += 16;
  }
  // A list out of order or with duplicates did not come from encode_progression.
  if (!std::is_sorted(decoded.perks.begin(), decoded.perks.end()) ||
      std::adjacent_find(decoded.perks.begin(), decoded.perks.end()) != decoded.perks.end()) {
    return false;
  }
  progression = std::move(decoded);
  return true;
}

// A server order. The perk-point bank of the main level, beside the tracks' indexes.
inline constexpr std::uint8_t kMainBank = 0xFF;
// XP an order may carry, in kXpUnits: a million XP, far above the 30 levels of any track.
inline constexpr std::uint32_t kMaxOrderXp = 1000000u * kXpUnits;

// Why a perk the player confirmed on the perk screen was not learnt (progressionPerkRefused).
enum class PerkRefusal : std::uint8_t {
  Points = 0,     // no point to spend in its tree
  Level = 1,      // its tree's level is too low
  Parent = 2,     // the perk it builds on is missing
  Exclusive = 3,  // its exclusive partner is owned
  Blocked = 4,    // the server blocks it
  Script = 5,     // a playerPerkLearning handler refused it
  Owned = 6,      // already owned
  Hidden = 7,     // not offered in this game
};
inline constexpr std::uint8_t kPerkRefusalCount = 8;

struct ProgressionOrder {
  enum class Kind : std::uint8_t {
    AddXp = 1,        // track, xp (kXpUnits), with the player's XP multipliers or without
    SetLevel = 2,     // track raised to level (the XP between the two, no multipliers)
    AddPerk = 3,      // perk
    RemovePerk = 4,   // perk
    AddPoints = 5,    // bank (a track index or kMainBank), count
    Respec = 6,       // the game's own respec
    Restore = 7,      // a whole snapshot, only ever raising (loadProgression)
    NativeXp = 8,     // whether XP from play may be earned at all (level != 0)
    LearnPerk = 9,    // perk: learnt as the perk screen learns it (a point spent)
    BlockPerks = 10,  // perks: the whole block list, which the game may not grant by itself
    RefusePerk = 11,  // perk, refusal: a perk the player asked for and the server turned down
    // track, level, xp (kXpUnits towards the next level): written as they are, down as well as
    // up, without the game's level-up path
    SetTrack = 12,
  };
  Kind kind = Kind::AddXp;
  std::uint8_t track = 0;  // AddXp, SetLevel; the bank of AddPoints
  std::uint32_t xp = 0;
  bool multipliers = false;
  std::uint8_t level = 0;
  std::uint16_t count = 0;
  PerkId perk{};
  std::vector<PerkId> perks;                  // BlockPerks
  PerkRefusal refusal = PerkRefusal::Points;  // RefusePerk
  Progression snapshot;                       // Restore
  // AddXp, SetLevel, Restore: whether the game announces the levels the order brings (its
  // "new level" notice); false holds the notice back ({ notify: false }).
  bool notify = true;
  // Restore: every track, perk and unspent point written exactly as the snapshot
  // has them, down as well as up, without the game's level-up path.
  bool exact = false;

  friend bool operator==(const ProgressionOrder& a, const ProgressionOrder& b) {
    return a.kind == b.kind && a.track == b.track && a.xp == b.xp &&
           a.multipliers == b.multipliers && a.level == b.level && a.count == b.count &&
           a.perk == b.perk && a.perks == b.perks && a.refusal == b.refusal &&
           a.snapshot == b.snapshot && a.notify == b.notify && a.exact == b.exact;
  }
};

namespace detail {

inline bool valid_bank(std::uint8_t bank) {
  return bank < kTracks.size() || bank == kMainBank;
}

}  // namespace detail

// PROGRESSION_ORDER: [tag][kind][payload]; empty for an order that cannot travel.
inline std::vector<std::uint8_t> encode_progression_order(const ProgressionOrder& order) {
  using Kind = ProgressionOrder::Kind;
  std::vector<std::uint8_t> p{kProgressionOrderTag, std::uint8_t(order.kind)};
  switch (order.kind) {
    case Kind::AddXp:
      if (order.track >= kTracks.size() || order.xp == 0 || order.xp > kMaxOrderXp) {
        return {};
      }
      p.push_back(order.track);
      detail::put_u32(p, order.xp);
      p.push_back(order.multipliers ? 1 : 0);
      p.push_back(order.notify ? 1 : 0);
      return p;
    case Kind::SetLevel:
      if (order.track >= kTracks.size() || order.level > kLevelCap) {
        return {};
      }
      p.push_back(order.track);
      p.push_back(order.level);
      p.push_back(order.notify ? 1 : 0);
      return p;
    case Kind::SetTrack:
      if (order.track >= kTracks.size() || order.level > kLevelCap || order.xp > kMaxOrderXp) {
        return {};
      }
      p.push_back(order.track);
      p.push_back(order.level);
      detail::put_u32(p, order.xp);
      return p;
    case Kind::AddPerk:
    case Kind::RemovePerk:
    case Kind::LearnPerk:
      p.insert(p.end(), order.perk.begin(), order.perk.end());
      return p;
    case Kind::RefusePerk:
      if (std::uint8_t(order.refusal) >= kPerkRefusalCount) {
        return {};
      }
      p.insert(p.end(), order.perk.begin(), order.perk.end());
      p.push_back(std::uint8_t(order.refusal));
      return p;
    case Kind::BlockPerks:
      if (order.perks.size() > kMaxPerks) {
        return {};
      }
      detail::put_u16(p, std::uint16_t(order.perks.size()));
      for (const PerkId& perk : order.perks) {
        p.insert(p.end(), perk.begin(), perk.end());
      }
      return p;
    case Kind::AddPoints:
      if (!detail::valid_bank(order.track) || order.count == 0) {
        return {};
      }
      p.push_back(order.track);
      detail::put_u16(p, order.count);
      return p;
    case Kind::Respec:
      return p;
    case Kind::NativeXp:
      p.push_back(order.level != 0 ? 1 : 0);
      return p;
    case Kind::Restore: {
      auto snapshot = encode_progression(order.snapshot);
      if (snapshot.empty()) {
        return {};
      }
      // Bit 0: notify; bit 1: exact.
      p.push_back(std::uint8_t((order.notify ? 1 : 0) | (order.exact ? 2 : 0)));
      p.insert(p.end(), snapshot.begin() + 1, snapshot.end());
      return p;
    }
  }
  return {};
}

inline bool decode_progression_order(const std::vector<std::uint8_t>& p, ProgressionOrder& order) {
  using Kind = ProgressionOrder::Kind;
  if (p.size() < 2 || p[0] != kProgressionOrderTag) {
    return false;
  }
  ProgressionOrder decoded;
  decoded.kind = Kind(p[1]);
  switch (decoded.kind) {
    case Kind::AddXp:
      if (p.size() != 2 + 1 + 4 + 1 + 1 || p[2] >= kTracks.size() || p[7] > 1 || p[8] > 1) {
        return false;
      }
      decoded.track = p[2];
      decoded.xp = detail::get_u32(p, 3);
      decoded.multipliers = p[7] == 1;
      decoded.notify = p[8] == 1;
      if (decoded.xp == 0 || decoded.xp > kMaxOrderXp) {
        return false;
      }
      break;
    case Kind::SetLevel:
      if (p.size() != 5 || p[2] >= kTracks.size() || p[3] > kLevelCap || p[4] > 1) {
        return false;
      }
      decoded.track = p[2];
      decoded.level = p[3];
      decoded.notify = p[4] == 1;
      break;
    case Kind::SetTrack:
      if (p.size() != 2 + 1 + 1 + 4 || p[2] >= kTracks.size() || p[3] > kLevelCap) {
        return false;
      }
      decoded.track = p[2];
      decoded.level = p[3];
      decoded.xp = detail::get_u32(p, 4);
      if (decoded.xp > kMaxOrderXp) {
        return false;
      }
      break;
    case Kind::AddPerk:
    case Kind::RemovePerk:
    case Kind::LearnPerk:
      if (p.size() != 2 + 16) {
        return false;
      }
      std::copy(p.begin() + 2, p.end(), decoded.perk.begin());
      break;
    case Kind::RefusePerk:
      if (p.size() != 2 + 16 + 1 || p[18] >= kPerkRefusalCount) {
        return false;
      }
      std::copy(p.begin() + 2, p.begin() + 18, decoded.perk.begin());
      decoded.refusal = PerkRefusal(p[18]);
      break;
    case Kind::BlockPerks: {
      if (p.size() < 4) {
        return false;
      }
      const std::size_t count = detail::get_u16(p, 2);
      if (count > kMaxPerks || p.size() != 4 + 16 * count) {
        return false;
      }
      decoded.perks.resize(count);
      for (std::size_t i = 0; i < count; ++i) {
        std::copy(p.begin() + std::ptrdiff_t(4 + 16 * i), p.begin() + std::ptrdiff_t(20 + 16 * i),
                  decoded.perks[i].begin());
      }
      break;
    }
    case Kind::AddPoints:
      if (p.size() != 5 || !detail::valid_bank(p[2])) {
        return false;
      }
      decoded.track = p[2];
      decoded.count = detail::get_u16(p, 3);
      if (decoded.count == 0) {
        return false;
      }
      break;
    case Kind::Respec:
      if (p.size() != 2) {
        return false;
      }
      break;
    case Kind::NativeXp:
      if (p.size() != 3 || p[2] > 1) {
        return false;
      }
      decoded.level = p[2];
      break;
    case Kind::Restore: {
      if (p.size() < 3 || p[2] > 3) {
        return false;
      }
      decoded.notify = (p[2] & 1) != 0;
      decoded.exact = (p[2] & 2) != 0;
      std::vector<std::uint8_t> snapshot{kProgressionTag};
      snapshot.insert(snapshot.end(), p.begin() + 3, p.end());
      if (!decode_progression(snapshot, decoded.snapshot)) {
        return false;
      }
      break;
    }
    default:
      return false;
  }
  order = std::move(decoded);
  return true;
}

// XP the player's own game produced and held back for the server.
inline constexpr std::size_t kMaxXpSourceBytes = 64;

struct XpGain {
  std::uint8_t track = 0;    // a kTracks index
  std::uint32_t xp = 0;      // in kXpUnits, before the player's multipliers
  bool multipliers = false;  // whether the game would have applied the player's multipliers
  std::string source;        // the game's cause: "Attack", "LockpickingResult", or empty

  friend bool operator==(const XpGain& a, const XpGain& b) {
    return a.track == b.track && a.xp == b.xp && a.multipliers == b.multipliers &&
           a.source == b.source;
  }
};

namespace detail {

// A cause name as XP_GAINING carries it: letters, digits and '_' only.
inline bool valid_source(const std::string& source) {
  if (source.size() > kMaxXpSourceBytes) {
    return false;
  }
  for (char c : source) {
    const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    if (!letter && !(c >= '0' && c <= '9') && c != '_') {
      return false;
    }
  }
  return true;
}

}  // namespace detail

// XP_GAINING: [tag][track][u32 xp][multipliers][source length][source]; empty when it cannot
// travel.
inline std::vector<std::uint8_t> encode_xp_gaining(const XpGain& gain) {
  if (gain.track >= kTracks.size() || gain.xp == 0 || gain.xp > kMaxOrderXp ||
      !detail::valid_source(gain.source)) {
    return {};
  }
  std::vector<std::uint8_t> p{kXpGainingTag, gain.track};
  detail::put_u32(p, gain.xp);
  p.push_back(gain.multipliers ? 1 : 0);
  p.push_back(std::uint8_t(gain.source.size()));
  p.insert(p.end(), gain.source.begin(), gain.source.end());
  return p;
}

inline bool decode_xp_gaining(const std::vector<std::uint8_t>& p, XpGain& gain) {
  if (p.size() < 8 || p[0] != kXpGainingTag || p[1] >= kTracks.size() || p[6] > 1 ||
      p.size() != std::size_t(8 + p[7])) {
    return false;
  }
  XpGain decoded;
  decoded.track = p[1];
  decoded.xp = detail::get_u32(p, 2);
  decoded.multipliers = p[6] == 1;
  decoded.source.assign(p.begin() + 8, p.end());
  if (decoded.xp == 0 || decoded.xp > kMaxOrderXp || !detail::valid_source(decoded.source)) {
    return false;
  }
  gain = std::move(decoded);
  return true;
}

// PERK_LEARNING: [tag][16 bytes of the perk's GUID].
inline std::vector<std::uint8_t> encode_perk_learning(const PerkId& perk) {
  std::vector<std::uint8_t> p{kPerkLearningTag};
  p.insert(p.end(), perk.begin(), perk.end());
  return p;
}

inline bool decode_perk_learning(const std::vector<std::uint8_t>& p, PerkId& perk) {
  if (p.size() != 17 || p[0] != kPerkLearningTag) {
    return false;
  }
  std::copy(p.begin() + 1, p.end(), perk.begin());
  return true;
}

}  // namespace kcd2s::integration
